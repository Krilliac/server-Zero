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
 * @file ClientSocketMgr.cpp
 * @brief Cluster gateway client socket manager implementation
 *
 * Slimmed-down clone of WorldSocketMgr: a thread pool driving an
 * ACE_TP_Reactor that accepts inbound game-client connections through an
 * ACE_Acceptor<ClientSocket>.
 */

#include "Common.h"
#include "Log.h"
#include "Config/Config.h"

#include "ClientSocketMgr.h"
#include "ClientSocket.h"

#include <ace/ACE.h>
#include <ace/Reactor.h>
#include <ace/TP_Reactor.h>
#include <ace/os_include/netinet/os_tcp.h>
#include <ace/os_include/sys/os_socket.h>

ClientSocketMgr::ClientSocketMgr()
    : m_SockOutUBuff(65536), m_UseNoDelay(true),
    reactor_(NULL), acceptor_(NULL)
{
}

ClientSocketMgr::~ClientSocketMgr()
{
    if (acceptor_)
    {
        delete acceptor_;
    }
    if (reactor_)
    {
        delete reactor_;
    }
}

/**
 * @brief Reactor thread body. Runs the event loop until told to stop.
 */
int ClientSocketMgr::svc()
{
    DEBUG_LOG("Gateway: starting client network thread");

    reactor_->run_reactor_event_loop();

    DEBUG_LOG("Gateway: client network thread exiting");
    return 0;
}

/**
 * @brief Create the reactor + acceptor and spawn the reactor threads.
 *
 * @param addr Address/port to bind the client acceptor to.
 * @return int 0 on success, -1 on failure.
 */
int ClientSocketMgr::StartNetwork(ACE_INET_Addr& addr)
{
    int num_threads = sConfig.GetIntDefault("Network.Threads", 1);
    if (num_threads <= 0)
    {
        sLog.outError("Network.Threads is wrong in your config file");
        return -1;
    }

    m_SockOutUBuff = sConfig.GetIntDefault("Network.OutUBuff", 65536);
    if (m_SockOutUBuff <= 0)
    {
        sLog.outError("Network.OutUBuff is wrong in your config file");
        return -1;
    }

    m_UseNoDelay = sConfig.GetBoolDefault("Network.TcpNodelay", true);

    // Thread-pool reactor so multiple threads can service connections.
    ACE_Reactor_Impl* imp = new ACE_TP_Reactor();
    imp->max_notify_iterations(128);
    reactor_ = new ACE_Reactor(imp, 1);

    acceptor_ = new ClientAcceptor;

    if (acceptor_->open(addr, reactor_, ACE_NONBLOCK) == -1)
    {
        sLog.outError("Gateway: failed to open client acceptor, check if the port is free");
        return -1;
    }

    if (activate(THR_NEW_LWP | THR_JOINABLE, num_threads) == -1)
    {
        sLog.outError("Gateway: failed to spawn client network threads");
        return -1;
    }

    sLog.outString("Gateway: client network started (%d thread(s), max handles %d)",
        num_threads, ACE::max_handles());
    return 0;
}

/**
 * @brief Stop accepting, end the reactor loop and join the threads.
 */
void ClientSocketMgr::StopNetwork()
{
    if (acceptor_)
    {
        acceptor_->close();
    }
    if (reactor_)
    {
        reactor_->end_reactor_event_loop();
    }
    wait();
}

/**
 * @brief Configure a newly accepted socket and bind it to the reactor.
 *
 * @param sock The freshly accepted socket.
 * @return int 0 on success, -1 on failure.
 */
int ClientSocketMgr::OnSocketOpen(ClientSocket* sock)
{
    static const int ndoption = 1;

    if (m_UseNoDelay)
    {
        if (sock->peer().set_option(ACE_IPPROTO_TCP, TCP_NODELAY, (void*)&ndoption, sizeof(int)) == -1)
        {
            sLog.outError("ClientSocketMgr::OnSocketOpen: peer().set_option TCP_NODELAY errno = %s", ACE_OS::strerror(errno));
            return -1;
        }
    }

    sock->m_OutBufferSize = static_cast<size_t>(m_SockOutUBuff);
    sock->reactor(reactor_);

    return 0;
}
