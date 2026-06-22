/**
 * MaNGOS is a full featured server for World of Warcraft, supporting
 * the following clients: 1.12.x, 2.4.3, 3.3.5a, 4.3.4a and 5.4.8
 *
 * Copyright (C) 2005-2026 MaNGOS <https://www.getmangos.eu>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * World of Warcraft, and all World of Warcraft or Warcraft art, images,
 * and lore are copyrighted by Blizzard Entertainment, Inc.
 */

/**
 * @file NodeLink.cpp
 * @brief Gateway-side TCP link to a backend mangosd node (implementation).
 *
 * Mirrors the game's ClusterThread: a dedicated thread owns one outbound
 * ACE_SOCK_Stream, (re)connects to the node with a short backoff, then blocks
 * on recv() and reassembles [uint32 len][uint8 type][payload] frames. Inbound
 * GW_CLIENT_PACKET frames are routed to the owning ClientSocket by clientId.
 */

#include <ace/SOCK_Connector.h>
#include <ace/INET_Addr.h>
#include <ace/Guard_T.h>
#include <ace/OS_NS_unistd.h>

#include "NodeLink.h"
#include "ClientSocket.h"
#include "Cluster/GatewayProtocol.h"

#include "Log.h"

NodeLink::NodeLink()
    : m_host(), m_port(0), m_secret(), m_running(false), m_connected(false)
{
}

NodeLink::~NodeLink()
{
    Stop();
}

int NodeLink::Start(const std::string& host, uint16 port, const std::string& secret)
{
    m_host    = host;
    m_port    = port;
    m_secret  = secret;
    m_running = true;

    if (activate(THR_NEW_LWP | THR_JOINABLE, 1) == -1)
    {
        sLog.outError("NodeLink: failed to spawn link thread");
        m_running = false;
        return -1;
    }
    return 0;
}

void NodeLink::Stop()
{
    if (!m_running)
    {
        return;
    }

    m_running = false;
    dropConnection(); // unblock a pending recv()
    wait();           // join the link thread
}

void NodeLink::RegisterClient(uint32 clientId, ClientSocket* sock)
{
    ACE_GUARD(ACE_Thread_Mutex, guard, m_clientsMutex);
    m_clients[clientId] = sock;
}

void NodeLink::UnregisterClient(uint32 clientId)
{
    ACE_GUARD(ACE_Thread_Mutex, guard, m_clientsMutex);
    m_clients.erase(clientId);
}

bool NodeLink::SendFrame(uint8 type, ByteBuffer const& payload)
{
    if (!m_connected)
    {
        return false;
    }

    ByteBuffer frame;
    GatewayFrame::Build(frame, type, payload);

    ACE_GUARD_RETURN(ACE_Thread_Mutex, guard, m_sendMutex, false);

    if (!m_connected)
    {
        return false;
    }

    ssize_t sent = m_stream.send_n(frame.contents(), frame.size());
    if (sent <= 0 || (size_t)sent != frame.size())
    {
        // The link thread will notice on its next recv() and reconnect.
        return false;
    }
    return true;
}

bool NodeLink::connectToNode()
{
    sLog.outString("NodeLink: connecting to %s:%u", m_host.c_str(), m_port);

    ACE_INET_Addr addr((u_short)m_port, m_host.c_str());
    ACE_SOCK_Connector connector;
    ACE_Time_Value timeout(2, 0); // 2s connect timeout

    if (connector.connect(m_stream, addr, &timeout) == -1)
    {
        sLog.outError("NodeLink: connection to node %s:%u failed (%s); will retry",
                      m_host.c_str(), m_port, ACE_OS::strerror(errno));
        return false;
    }

    m_connected = true;
    m_recvBuf.clear();
    sLog.outString("NodeLink: connected to node %s:%u", m_host.c_str(), m_port);
    return true;
}

bool NodeLink::sendHello()
{
    // GW_HELLO must be the very first frame on the link: string secret, then the
    // protocol version. The node validates the secret (constant-time) before it
    // will honor any session frame; until then nothing else is sent.
    ByteBuffer payload;
    payload << m_secret;
    payload << (uint32)GW_PROTOCOL_VERSION;

    if (!SendFrame((uint8)GW_HELLO, payload))
    {
        sLog.outError("NodeLink: failed to send GW_HELLO to node %s:%u", m_host.c_str(), m_port);
        return false;
    }
    sLog.outString("NodeLink: sent GW_HELLO (link authentication) to node %s:%u", m_host.c_str(), m_port);
    return true;
}

void NodeLink::dropConnection()
{
    ACE_GUARD(ACE_Thread_Mutex, guard, m_sendMutex);
    if (m_connected)
    {
        m_connected = false;
        m_stream.close();
    }
}

void NodeLink::receiveLoop()
{
    uint8 tmp[8192];
    while (m_running && m_connected)
    {
        ssize_t got = m_stream.recv(tmp, sizeof(tmp));
        if (got <= 0)
        {
            // peer closed / error
            break;
        }
        m_recvBuf.insert(m_recvBuf.end(), tmp, tmp + got);
        parseFrames();
    }
}

void NodeLink::parseFrames()
{
    size_t off = 0;
    while (m_recvBuf.size() - off >= GatewayFrame::HEADER_SIZE)
    {
        const uint8* p = &m_recvBuf[off];
        uint32 len = (uint32)p[0] | ((uint32)p[1] << 8) | ((uint32)p[2] << 16) | ((uint32)p[3] << 24);
        uint8  type = p[4];

        if (len > GatewayFrame::MAX_PAYLOAD)
        {
            sLog.outError("NodeLink: oversized frame (%u bytes); dropping connection", len);
            m_recvBuf.clear();
            return;
        }
        if (m_recvBuf.size() - off < GatewayFrame::HEADER_SIZE + len)
        {
            break; // wait for the rest
        }

        const uint8* payload = p + GatewayFrame::HEADER_SIZE;
        dispatch(type, payload, len);
        off += GatewayFrame::HEADER_SIZE + len;
    }

    if (off)
    {
        m_recvBuf.erase(m_recvBuf.begin(), m_recvBuf.begin() + off);
    }
}

void NodeLink::dispatch(uint8 type, const uint8* payload, uint32 len)
{
    if (type != GW_CLIENT_PACKET)
    {
        // GW_SESSION_* and the reserved migration types carry no client packet
        // to route here in Phase 1.
        return;
    }

    // GW_CLIENT_PACKET payload: uint32 clientId, uint16 opcode, raw bytes.
    if (len < 6)
    {
        sLog.outError("NodeLink: truncated GW_CLIENT_PACKET (%u bytes)", len);
        return;
    }

    ByteBuffer in;
    in.append(payload, len);

    uint32 clientId = 0;
    uint16 opcode = 0;
    in >> clientId;
    in >> opcode;

    const size_t bodyLen = len - 6;
    ByteBuffer body;
    if (bodyLen)
    {
        body.append(payload + 6, bodyLen);
    }

    // Route to the owning client. Note: ClientSocket is reference-counted and
    // owned by the reactor; we only deliver through it while it is registered.
    ClientSocket* sock = NULL;
    {
        ACE_GUARD(ACE_Thread_Mutex, guard, m_clientsMutex);
        std::map<uint32, ClientSocket*>::iterator it = m_clients.find(clientId);
        if (it != m_clients.end())
        {
            sock = it->second;
        }
    }

    if (!sock)
    {
        // Client gone (released) before the node's reply arrived; drop it.
        return;
    }

    sock->DeliverServerPacket(opcode, body);
}

int NodeLink::svc()
{
    sLog.outString("NodeLink: link thread started (target %s:%u)", m_host.c_str(), m_port);

    while (m_running)
    {
        if (!connectToNode())
        {
            // Backoff before retrying so a down node can't spin the thread.
            for (int i = 0; i < 30 && m_running; ++i)
            {
                ACE_OS::sleep(ACE_Time_Value(0, 100000)); // 100 ms slices, ~3s total
            }
            continue;
        }

        // Authenticate the link FIRST: send GW_HELLO before any session traffic.
        // If it fails the connection is already broken; drop and reconnect.
        if (!sendHello())
        {
            dropConnection();
            continue;
        }

        receiveLoop();
        dropConnection();

        if (m_running)
        {
            sLog.outString("NodeLink: connection to node %s:%u lost; reconnecting",
                           m_host.c_str(), m_port);
        }
    }

    dropConnection();
    sLog.outString("NodeLink: link thread stopped");
    return 0;
}

// ---------------------------------------------------------------------------
// Process-wide single instance (Phase 1: one fixed node).
// ---------------------------------------------------------------------------
NodeLink* GetNodeLink()
{
    static NodeLink instance;
    return &instance;
}
