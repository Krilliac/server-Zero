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

#include <ace/Reactor.h>
#include <ace/TP_Reactor.h>
#include <ace/Svc_Handler.h>
#include <ace/Acceptor.h>
#include <ace/SOCK_Acceptor.h>
#include <ace/SOCK_Stream.h>
#include <ace/Synch_Traits.h>

#include "GatewayIntake.h"
#include "GatewayProtocol.h"
#include "WorldSession.h"
#include "WorldPacket.h"
#include "World.h"
#include "Opcodes.h"
#include "Log.h"
#include "ByteBuffer.h"
#include "SharedDefines.h"

// ---------------------------------------------------------------------------
// GatewayLink — the gateway's accepted connection (one per socket; in practice
// a single active link). Parses the [uint32 len][uint8 type][payload] frame
// stream and applies each frame. Runs on the intake network thread.
// ---------------------------------------------------------------------------
class GatewayLink : public ACE_Svc_Handler<ACE_SOCK_STREAM, ACE_NULL_SYNCH>
{
        typedef ACE_Svc_Handler<ACE_SOCK_STREAM, ACE_NULL_SYNCH> Base;

    public:
        friend class ACE_Acceptor<GatewayLink, ACE_SOCK_ACCEPTOR>;

        GatewayLink() : Base()
        {
            reference_counting_policy().value(ACE_Event_Handler::Reference_Counting_Policy::ENABLED);
        }

        int open(void* /*unused*/) override
        {
            if (reactor()->register_handler(this, ACE_Event_Handler::READ_MASK) == -1)
            {
                sLog.outError("GatewayLink::open: register_handler failed");
                return -1;
            }

            ACE_INET_Addr remote;
            if (peer().get_remote_addr(remote) != -1)
            {
                sLog.outString("Gateway intake: accepted gateway connection from %s:%u",
                               remote.get_host_addr(), remote.get_port_number());
            }
            else
            {
                sLog.outString("Gateway intake: accepted gateway connection");
            }

            sGatewayIntake.SetActiveLink(this);
            return 0;
        }

        int handle_input(ACE_HANDLE = ACE_INVALID_HANDLE) override
        {
            uint8 tmp[8192];
            ssize_t got = peer().recv(tmp, sizeof(tmp));
            if (got <= 0)
                return -1; // peer closed / error -> reactor closes us

            m_buf.insert(m_buf.end(), tmp, tmp + got);
            parseFrames();
            return 0;
        }

        int handle_close(ACE_HANDLE h = ACE_INVALID_HANDLE,
                         ACE_Reactor_Mask mask = ACE_Event_Handler::ALL_EVENTS_MASK) override
        {
            sGatewayIntake.OnLinkClosed(this);
            return Base::handle_close(h, mask);
        }

        // Write a complete framed buffer to the gateway. Called on the network
        // thread (from GatewayIntake::flushOutbound).
        bool sendRaw(const uint8* bytes, size_t len)
        {
            return peer().send_n(bytes, len) > 0;
        }

    private:
        void parseFrames()
        {
            size_t off = 0;
            while (m_buf.size() - off >= GatewayFrame::HEADER_SIZE)
            {
                const uint8* p = &m_buf[off];
                uint32 len = (uint32)p[0] | ((uint32)p[1] << 8) | ((uint32)p[2] << 16) | ((uint32)p[3] << 24);
                uint8  type = p[4];

                if (len > GatewayFrame::MAX_PAYLOAD)
                {
                    sLog.outError("GatewayLink: oversized frame (%u bytes); dropping connection", len);
                    m_buf.clear();
                    return;
                }
                if (m_buf.size() - off < GatewayFrame::HEADER_SIZE + len)
                    break; // wait for the rest

                const uint8* payload = p + GatewayFrame::HEADER_SIZE;
                dispatch(type, payload, len);
                off += GatewayFrame::HEADER_SIZE + len;
            }

            if (off)
                m_buf.erase(m_buf.begin(), m_buf.begin() + off);
        }

        void dispatch(uint8 type, const uint8* payload, uint32 len)
        {
            // Wrap the raw payload in a ByteBuffer for safe, endian-correct reads.
            ByteBuffer in;
            if (len)
                in.append(payload, len);

            try
            {
                switch (type)
                {
                    case GW_SESSION_OPEN:    handleSessionOpen(in);    break;
                    case GW_CLIENT_PACKET:   handleClientPacket(in);   break;
                    case GW_SESSION_RELEASE: handleSessionRelease(in); break;
                    default:
                        // Reserved/unsupported types are ignored in this phase.
                        DEBUG_LOG("GatewayLink: ignoring frame type %u (%u bytes)", type, len);
                        break;
                }
            }
            catch (ByteBufferException&)
            {
                sLog.outError("GatewayLink: malformed frame (type %u, %u bytes) from gateway", type, len);
            }
        }

        // GW_SESSION_OPEN: uint32 clientId, uint32 accountId, uint32 security,
        //                  uint8 locale, string accountName
        void handleSessionOpen(ByteBuffer& in)
        {
            uint32 clientId;  in >> clientId;
            uint32 accountId; in >> accountId;
            uint32 security;  in >> security;
            uint8  locale;    in >> locale;
            std::string accountName; in >> accountName;

            if (sGatewayIntake.FindSession(clientId))
            {
                sLog.outError("Gateway intake: GW_SESSION_OPEN for already-open clientId %u (account %u); ignoring",
                              clientId, accountId);
                return;
            }

            if (security > SEC_ADMINISTRATOR)
                security = SEC_ADMINISTRATOR;
            LocaleConstant loc = locale >= MAX_LOCALE ? LOCALE_enUS : LocaleConstant(locale);

            // Pre-authed plaintext session: no client WorldSocket, no AuthCrypt.
            // The gateway already authed the client (SRP6), so we trust the
            // account id/security/locale carried in the frame and mark the
            // session AUTHED by routing it straight into the world like a normal
            // post-auth WorldSocket would (sWorld.AddSession). STATUS_AUTHED is
            // implicit: AddSession() admits the session into the world session
            // map and its recv queue is then served by the standard opcode
            // dispatch in WorldSession::Update() (which honors per-opcode
            // STATUS_* requirements). There is no auth queue to skip here.
            WorldSession* session = new WorldSession(accountId, NULL, AccountTypes(security), 0, loc);
            session->SetGatewayFronted(clientId);
            session->LoadTutorialsData();

            sGatewayIntake.RegisterSession(clientId, session);
            sWorld.AddSession(session);

            sLog.outString("Gateway intake: opened fronted session clientId %u (account %u '%s', security %u)",
                           clientId, accountId, accountName.c_str(), security);
        }

        // GW_CLIENT_PACKET: uint32 clientId, uint16 opcode, raw packet bytes
        void handleClientPacket(ByteBuffer& in)
        {
            uint32 clientId; in >> clientId;
            uint16 opcode;   in >> opcode;

            WorldSession* session = sGatewayIntake.FindSession(clientId);
            if (!session)
            {
                DEBUG_LOG("GatewayLink: GW_CLIENT_PACKET for unknown clientId %u (opcode 0x%04X)", clientId, opcode);
                return;
            }

            if (opcode >= NUM_MSG_TYPES)
            {
                sLog.outError("GatewayLink: GW_CLIENT_PACKET nonexistent opcode 0x%04X for clientId %u", opcode, clientId);
                return;
            }

            // Remaining bytes are the (already decrypted) client packet payload.
            size_t remaining = in.size() - in.rpos();
            WorldPacket* pkt = new WorldPacket(opcode, remaining);
            if (remaining)
                pkt->append(in.contents() + in.rpos(), remaining);

            // Deliver exactly as WorldSocket::ProcessIncoming does for an authed
            // packet, so the normal opcode handlers run on the world thread.
            session->QueuePacket(pkt);
        }

        // GW_SESSION_RELEASE: uint32 clientId
        void handleSessionRelease(ByteBuffer& in)
        {
            uint32 clientId; in >> clientId;
            sGatewayIntake.ReleaseSession(clientId);
            sLog.outString("Gateway intake: released fronted session clientId %u", clientId);
        }

        std::vector<uint8> m_buf;
};

// ---------------------------------------------------------------------------
// GatewayIntake
// ---------------------------------------------------------------------------
GatewayIntake& GatewayIntake::Instance()
{
    static GatewayIntake s_instance;
    return s_instance;
}

GatewayIntake::GatewayIntake()
    : m_reactor(NULL), m_acceptor(NULL), m_listenAddr(), m_running(false),
      m_port(0), m_activeLink(NULL)
{
}

GatewayIntake::~GatewayIntake()
{
    Stop();
}

bool GatewayIntake::Start(uint16 port)
{
    if (port == 0)
        return false; // gated off

    if (m_running)
        return true;

    m_port = port;
    if (m_listenAddr.set((u_short)port, (ACE_UINT32)INADDR_ANY) == -1)
    {
        sLog.outError("Gateway intake: cannot build listen address for port %u", port);
        return false;
    }

    ACE_Reactor_Impl* imp = new ACE_TP_Reactor();
    imp->max_notify_iterations(128);
    m_reactor = new ACE_Reactor(imp, 1);

    m_acceptor = new GatewayAcceptor;
    if (m_acceptor->open(m_listenAddr, m_reactor, ACE_NONBLOCK) == -1)
    {
        sLog.outError("Gateway intake: cannot bind intake listener to port %u (is it free?)", port);
        delete m_acceptor; m_acceptor = NULL;
        delete m_reactor;  m_reactor = NULL;
        return false;
    }

    m_running = true;
    if (activate() == -1)
    {
        sLog.outError("Gateway intake: failed to activate listener thread");
        m_running = false;
        m_acceptor->close();
        delete m_acceptor; m_acceptor = NULL;
        delete m_reactor;  m_reactor = NULL;
        return false;
    }

    sLog.outString("Gateway intake: listening for the cluster gateway on port %u (plaintext pre-authed sessions).", port);
    return true;
}

void GatewayIntake::Stop()
{
    if (!m_running)
        return;

    m_running = false;
    if (m_reactor)
        m_reactor->end_reactor_event_loop();
    wait();

    // Release any sessions still fronted by the gateway.
    {
        std::lock_guard<std::mutex> guard(m_sessionLock);
        for (std::map<uint32, WorldSession*>::iterator it = m_sessions.begin(); it != m_sessions.end(); ++it)
        {
            if (it->second)
                it->second->ClearGatewayFronted();
        }
        m_sessions.clear();
    }

    {
        std::lock_guard<std::mutex> guard(m_linkLock);
        m_activeLink = NULL;
    }

    if (m_acceptor)
    {
        m_acceptor->close();
        delete m_acceptor;
        m_acceptor = NULL;
    }
    if (m_reactor)
    {
        delete m_reactor;
        m_reactor = NULL;
    }

    sLog.outString("Gateway intake: listener stopped.");
}

int GatewayIntake::svc()
{
    sLog.outString("Gateway intake: network thread started (listening on port %u).", m_port);

    while (m_running && !m_reactor->reactor_event_loop_done())
    {
        ACE_Time_Value interval(0, 100000); // 100 ms reactor slice
        m_reactor->run_reactor_event_loop(interval);

        if (World::IsStopped() || !m_running)
            break;

        flushOutbound();
    }

    if (m_acceptor)
        m_acceptor->close();

    sLog.outString("Gateway intake: network thread stopped.");
    return 0;
}

// ---- world thread ---------------------------------------------------------

void GatewayIntake::SendToClient(uint32 clientId, uint16 opcode, const uint8* data, uint32 len)
{
    if (!m_running)
        return;

    // Build the GW_CLIENT_PACKET payload: uint32 clientId, uint16 opcode, raw bytes.
    ByteBuffer payload;
    payload << clientId;
    payload << opcode;
    if (len && data)
        payload.append(data, len);

    // Frame it (header + payload) and enqueue the raw on-wire bytes.
    ByteBuffer frame;
    GatewayFrame::Build(frame, (uint8)GW_CLIENT_PACKET, payload);

    std::vector<uint8> bytes;
    if (frame.size())
        bytes.assign(frame.contents(), frame.contents() + frame.size());

    std::lock_guard<std::mutex> guard(m_outLock);
    m_outQueue.push_back(bytes);
}

// ---- network thread -------------------------------------------------------

void GatewayIntake::flushOutbound()
{
    std::deque<std::vector<uint8> > pending;
    {
        std::lock_guard<std::mutex> guard(m_outLock);
        if (m_outQueue.empty())
            return;
        pending.swap(m_outQueue);
    }

    GatewayLink* link;
    {
        std::lock_guard<std::mutex> guard(m_linkLock);
        link = m_activeLink;
    }

    if (!link)
        return; // gateway not connected; drop (sessions will be released on close)

    for (std::deque<std::vector<uint8> >::iterator it = pending.begin(); it != pending.end(); ++it)
    {
        if (it->empty())
            continue;
        if (!link->sendRaw(&(*it)[0], it->size()))
        {
            // Send failed; the link's handle_close will release sessions.
            break;
        }
    }
}

void GatewayIntake::RegisterSession(uint32 clientId, WorldSession* session)
{
    std::lock_guard<std::mutex> guard(m_sessionLock);
    m_sessions[clientId] = session;
}

WorldSession* GatewayIntake::FindSession(uint32 clientId)
{
    std::lock_guard<std::mutex> guard(m_sessionLock);
    std::map<uint32, WorldSession*>::iterator it = m_sessions.find(clientId);
    return it != m_sessions.end() ? it->second : NULL;
}

void GatewayIntake::ReleaseSession(uint32 clientId)
{
    WorldSession* session = NULL;
    {
        std::lock_guard<std::mutex> guard(m_sessionLock);
        std::map<uint32, WorldSession*>::iterator it = m_sessions.find(clientId);
        if (it != m_sessions.end())
        {
            session = it->second;
            m_sessions.erase(it);
        }
    }

    // Marking the session no longer fronted lets WorldSession::Update() run the
    // normal !connected logout path on the world thread (saves + removes it).
    if (session)
        session->ClearGatewayFronted();
}

void GatewayIntake::SetActiveLink(GatewayLink* link)
{
    std::lock_guard<std::mutex> guard(m_linkLock);
    m_activeLink = link;
}

void GatewayIntake::OnLinkClosed(GatewayLink* link)
{
    {
        std::lock_guard<std::mutex> guard(m_linkLock);
        if (m_activeLink == link)
            m_activeLink = NULL;
        else
            return; // not the active link; nothing to release
    }

    // The gateway dropped: release every fronted session so they log out cleanly.
    std::map<uint32, WorldSession*> dropped;
    {
        std::lock_guard<std::mutex> guard(m_sessionLock);
        dropped.swap(m_sessions);
    }
    for (std::map<uint32, WorldSession*>::iterator it = dropped.begin(); it != dropped.end(); ++it)
    {
        if (it->second)
            it->second->ClearGatewayFronted();
    }

    sLog.outString("Gateway intake: gateway connection closed; released %zu fronted session(s).", dropped.size());
}
