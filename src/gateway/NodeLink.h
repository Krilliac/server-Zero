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
 * @file NodeLink.h
 * @brief Gateway-side TCP link to a backend mangosd node.
 *
 * NodeLink owns ONE outbound TCP connection from the gateway to a node's
 * intake. Phase 2 runs N of them (one per configured node), each carrying a
 * node id; the NodeRegistry owns the instances. It mirrors the connect +
 * framing model of the game's ClusterThread (ClusterNetwork):
 *   - a dedicated thread (ACE_Task_Base) owns the socket,
 *   - outbound frames use [uint32 len][uint8 type][payload] (GatewayFrame),
 *   - the same thread blocks on recv(), reassembles inbound frames and
 *     dispatches GW_CLIENT_PACKET to the owning ClientSocket by clientId,
 *     looked up via the registry's SHARED clientId -> socket map (a client's
 *     inbound packets can come from whichever node currently fronts it).
 *
 * Sends may come from reactor threads, so SendFrame() is guarded by a mutex.
 * If the node is down, SendFrame() simply reports failure (gateway keeps
 * running) and the thread keeps retrying the connect with a fixed backoff.
 */

#ifndef GATEWAY_H_NODELINK
#define GATEWAY_H_NODELINK

#include <ace/SOCK_Stream.h>
#include <ace/Task.h>
#include <ace/Thread_Mutex.h>

#include "Common.h"
#include "ByteBuffer.h"

#include <map>
#include <string>
#include <vector>

class ClientSocket;

/**
 * @brief Manages one gateway->node TCP connection on its own thread.
 */
class NodeLink : public ACE_Task_Base
{
    public:
        /// Build a link to one node. host/port/secret are fixed for the life of
        /// the link; nodeId is the registry key used in routing/logging.
        NodeLink(uint32 nodeId, const std::string& host, uint16 port, const std::string& secret);
        virtual ~NodeLink();

        /// Start the link thread. The thread keeps (re)connecting until Stop()
        /// is called; the secret is sent as the GW_HELLO first frame on every
        /// (re)connect to authenticate the link. Returns 0 on success.
        int Start();

        /// Request the link thread to exit and join it.
        void Stop();

        /// Registry key / wire id for this node.
        uint32 NodeId() const { return m_NodeId; }

        /// True while the outbound socket is connected to the node.
        bool IsConnected() const { return m_connected; }

        /// Build a [len][type][payload] frame and send it to the node.
        /// Thread-safe. Returns true on a full send, false if not connected
        /// or the send failed (caller treats failure as best-effort).
        bool SendFrame(uint8 type, ByteBuffer const& payload);

        /// ACE_Task_Base entry point (the link thread body).
        virtual int svc() override;

    private:
        bool connectToNode();          // one blocking connect attempt
        bool sendHello();              // send GW_HELLO{secret} as the first frame
        void dropConnection();         // close + mark disconnected
        void receiveLoop();            // blocking recv + frame reassembly
        void parseFrames();            // consume complete frames from m_recvBuf
        void dispatch(uint8 type, const uint8* payload, uint32 len);

        uint32        m_NodeId;        // registry key / wire id for this node
        std::string   m_host;
        uint16        m_port;
        std::string   m_secret;        // pre-shared link secret sent in GW_HELLO
        volatile bool m_running;
        volatile bool m_connected;

        ACE_SOCK_Stream m_stream;       // outbound socket to the node
        mutable ACE_Thread_Mutex m_sendMutex; // guards m_stream sends

        std::vector<uint8> m_recvBuf;   // inbound reassembly (link thread only)
};

#endif /* GATEWAY_H_NODELINK */
