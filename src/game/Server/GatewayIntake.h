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
 * @file GatewayIntake.h
 * @brief Cluster gateway intake (Task 6): the node side of the gateway tunnel.
 *
 * Accepts the cluster gateway's internal TCP connection on Gateway.IntakePort
 * and runs PRE-AUTHED, PLAINTEXT WorldSessions fed by the gateway. The gateway
 * has already performed the SRP6 client auth, so there is NO per-node AuthCrypt
 * here: client packets arrive already decrypted as GW_CLIENT_PACKET frames and
 * are injected into a normal WorldSession via QueuePacket() so the standard
 * opcode handlers run. Outgoing server packets for a fronted session are framed
 * back over the same connection (see WorldSession::SendPacket).
 *
 * This is entirely additive and gated: nothing here runs unless
 * Gateway.IntakePort > 0. Mirrors the Cluster inter-node ClusterThread: a
 * dedicated ACE_Task_Base thread with its OWN reactor (off the world reactor)
 * and an ACE_Acceptor of GatewayLink handlers.
 *
 * Thread boundaries:
 *  - Network thread: accept, parse frames, create/release fronted sessions
 *    (sWorld.AddSession is queue-based and thread-safe), QueuePacket (the recv
 *    queue is a LockedQueue), and flush the outbound byte queue to the gateway.
 *  - World thread: WorldSession::SendPacket only enqueues outbound bytes; it
 *    never touches the socket directly.
 */

#ifndef MANGOS_GATEWAYINTAKE_H
#define MANGOS_GATEWAYINTAKE_H

#include "Common.h"

#include <ace/Task.h>
#include <ace/INET_Addr.h>

#include <map>
#include <deque>
#include <vector>
#include <mutex>

class GatewayLink;
class WorldSession;
class ACE_Reactor;

template <class T, class A> class ACE_Acceptor;
class ACE_SOCK_ACCEPTOR;

/**
 * @brief Singleton owner of the gateway intake listener thread.
 *
 * Constructed inert; Start(port) spins up the dedicated reactor thread, Stop()
 * tears it down. Access through the sGatewayIntake macro.
 */
class GatewayIntake : public ACE_Task_Base
{
    public:
        static GatewayIntake& Instance();

        /// Start the intake listener on the given port (no-op if port == 0 or
        /// already running). Returns true if the listener is up afterwards.
        bool Start(uint16 port);

        /// Stop the listener thread and tear down all fronted sessions. Safe to
        /// call when not running.
        void Stop();

        bool IsRunning() const { return m_running; }

        // ACE_Task_Base
        int svc() override;

        // ---- called from the world thread ----------------------------------
        // Enqueue an outgoing (server -> client) packet for a fronted session.
        // Just frames bytes into the thread-safe outbound queue; the network
        // thread writes them to the gateway connection.
        void SendToClient(uint32 clientId, uint16 opcode, const uint8* data, uint32 len);

        // ---- called from the network thread (GatewayLink) ------------------
        // Register / look up / drop fronted sessions by gateway clientId.
        void RegisterSession(uint32 clientId, WorldSession* session);
        WorldSession* FindSession(uint32 clientId);
        void ReleaseSession(uint32 clientId);
        // The active gateway link dropped: release every fronted session.
        void OnLinkClosed(GatewayLink* link);
        void SetActiveLink(GatewayLink* link);

    private:
        GatewayIntake();
        ~GatewayIntake();
        GatewayIntake(GatewayIntake const&) = delete;
        GatewayIntake& operator=(GatewayIntake const&) = delete;

        void flushOutbound();   // network thread: drain m_outQueue -> gateway link

        typedef ACE_Acceptor<GatewayLink, ACE_SOCK_ACCEPTOR> GatewayAcceptor;

        ACE_Reactor*     m_reactor;
        GatewayAcceptor* m_acceptor;
        ACE_INET_Addr    m_listenAddr;
        volatile bool    m_running;
        uint16           m_port;

        // clientId -> fronted WorldSession. Touched on the network thread only.
        std::map<uint32, WorldSession*> m_sessions;
        std::mutex                      m_sessionLock;

        // Outbound (server -> gateway) frames awaiting send. Written by the world
        // thread, drained by the network thread.
        std::deque<std::vector<uint8> > m_outQueue;
        std::mutex                      m_outLock;

        // The single active gateway connection. Set on accept, cleared on close.
        GatewayLink*    m_activeLink;
        std::mutex      m_linkLock;
};

#define sGatewayIntake GatewayIntake::Instance()

#endif // MANGOS_GATEWAYINTAKE_H
