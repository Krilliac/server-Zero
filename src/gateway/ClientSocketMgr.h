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
 * @file ClientSocketMgr.h
 * @brief Cluster gateway client socket manager
 *
 * Mirrors the world server's WorldSocketMgr: owns the ACE_TP_Reactor and the
 * ACE_Acceptor<ClientSocket>, runs a pool of reactor threads, and applies
 * per-socket options on accept. Singleton, accessed via sClientSocketMgr.
 */

#ifndef GATEWAY_H_CLIENTSOCKETMGR
#define GATEWAY_H_CLIENTSOCKETMGR

#include <ace/Basic_Types.h>
#include <ace/Singleton.h>
#include <ace/Thread_Mutex.h>
#include <ace/INET_Addr.h>
#include <ace/Task.h>
#include <ace/Acceptor.h>
#include <ace/SOCK_Acceptor.h>

#include "ClientSocket.h"

class ACE_Reactor;

/**
 * @brief Pool of reactor threads handling inbound game-client sockets.
 */
class ClientSocketMgr : public ACE_Task_Base
{
    friend class ACE_Singleton<ClientSocketMgr, ACE_Thread_Mutex>;
    friend class ClientSocket;

    public:
        /// Create the reactor + acceptor and spawn the reactor threads.
        /// @return 0 on success, -1 on failure
        int StartNetwork(ACE_INET_Addr& addr);

        /// Stop accepting, end the reactor loop and join the threads.
        void StopNetwork();

    private:
        /// Apply socket options on a freshly accepted socket and bind it
        /// to the reactor. Called from ClientSocket::open().
        int OnSocketOpen(ClientSocket* sock);

        /// Reactor thread body (one per network thread).
        virtual int svc();

        ClientSocketMgr();
        virtual ~ClientSocketMgr();

    private:
        int  m_SockOutUBuff; ///< Output user buffer size
        bool m_UseNoDelay;   ///< Use TCP_NODELAY

        ACE_Reactor*    reactor_;  ///< ACE reactor
        ClientAcceptor* acceptor_; ///< Client acceptor (ACE_Acceptor<ClientSocket>)
};

#define sClientSocketMgr ACE_Singleton<ClientSocketMgr, ACE_Thread_Mutex>::instance()

#endif /* GATEWAY_H_CLIENTSOCKETMGR */
