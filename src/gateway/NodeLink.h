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
 * @brief Gateway-side TCP link to a backend mangosd node (Phase 1: one node).
 *
 * NodeLink owns ONE outbound TCP connection from the gateway to a node's
 * intake (host/port from config Node.1.Host / Node.1.GatewayPort). It mirrors
 * the connect + framing model of the game's ClusterThread (ClusterNetwork):
 *   - a dedicated thread (ACE_Task_Base) owns the socket,
 *   - outbound frames use [uint32 len][uint8 type][payload] (GatewayFrame),
 *   - the same thread blocks on recv(), reassembles inbound frames and
 *     dispatches GW_CLIENT_PACKET to the owning ClientSocket by clientId.
 *
 * Sends may come from reactor threads, so SendFrame() is guarded by a mutex.
 * If the node is down, SendFrame() simply reports failure (gateway keeps
 * running) and the thread keeps retrying the connect with a fixed backoff.
 *
 * A single shared instance is held in NodeLink.cpp and reached via sNodeLink.
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
        NodeLink();
        virtual ~NodeLink();

        /// Configure (host/port) and start the link thread. The thread keeps
        /// (re)connecting until Stop() is called. Returns 0 on success.
        int Start(const std::string& host, uint16 port);

        /// Request the link thread to exit and join it.
        void Stop();

        /// True while the outbound socket is connected to the node.
        bool IsConnected() const { return m_connected; }

        /// Build a [len][type][payload] frame and send it to the node.
        /// Thread-safe. Returns true on a full send, false if not connected
        /// or the send failed (caller treats failure as best-effort).
        bool SendFrame(uint8 type, ByteBuffer const& payload);

        /// Register / unregister a client by its gateway-assigned id so that
        /// inbound node packets can be routed back to the right ClientSocket.
        void RegisterClient(uint32 clientId, ClientSocket* sock);
        void UnregisterClient(uint32 clientId);

        /// ACE_Task_Base entry point (the link thread body).
        virtual int svc() override;

    private:
        bool connectToNode();          // one blocking connect attempt
        void dropConnection();         // close + mark disconnected
        void receiveLoop();            // blocking recv + frame reassembly
        void parseFrames();            // consume complete frames from m_recvBuf
        void dispatch(uint8 type, const uint8* payload, uint32 len);

        std::string   m_host;
        uint16        m_port;
        volatile bool m_running;
        volatile bool m_connected;

        ACE_SOCK_Stream m_stream;       // outbound socket to the node
        mutable ACE_Thread_Mutex m_sendMutex; // guards m_stream sends

        std::vector<uint8> m_recvBuf;   // inbound reassembly (link thread only)

        // clientId -> ClientSocket*, guarded by m_clientsMutex.
        std::map<uint32, ClientSocket*> m_clients;
        ACE_Thread_Mutex m_clientsMutex;
};

/// Process-wide single NodeLink instance (Phase 1: one fixed node).
NodeLink* GetNodeLink();
#define sNodeLink (GetNodeLink())

#endif /* GATEWAY_H_NODELINK */
