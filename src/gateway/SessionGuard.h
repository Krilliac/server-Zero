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
 * @file SessionGuard.h
 * @brief Gateway-wide session-integrity registry (one-per-account + per-IP cap).
 *
 * The gateway is the single chokepoint that sees every connection, so it can
 * enforce cluster-wide session rules a single node cannot. This singleton holds
 *   accountId -> live ClientSocket*   (one live connection per account)
 *   ip(string) -> distinct-account count (accounts-per-IP cap)
 * Both maps are guarded by one mutex: reactor threads (multiple, one per CPU)
 * register at auth and unregister at close. No game headers; ClientSocket is an
 * opaque pointer used only as an identity/handle.
 */

#ifndef GATEWAY_H_SESSIONGUARD
#define GATEWAY_H_SESSIONGUARD

#include <ace/Thread_Mutex.h>

#include "Common.h"
#include "EdgeCheckConfig.h"

#include <map>
#include <set>
#include <string>

class ClientSocket;

class SessionGuard
{
    public:
        struct AdmitResult
        {
            bool          ipCapExceeded;      ///< this conn pushed the IP over cap
            bool          accountAlreadyLive; ///< another live conn held this account
            ClientSocket* previous;           ///< the prior holder (or NULL)
            AdmitResult() : ipCapExceeded(false), accountAlreadyLive(false), previous(NULL) {}
        };

        /// Copy the relevant tunables once at boot.
        void Configure(const EdgeCheckConfig& cfg);

        /// Record a freshly-authed connection. Returns the integrity verdicts.
        AdmitResult Register(uint32 accountId, const std::string& ip, ClientSocket* sock);

        /// Remove a connection (idempotent). Clears the account slot only if
        /// @p sock still owns it; decrements the IP's account count.
        void Unregister(uint32 accountId, const std::string& ip, ClientSocket* sock);

        /// True if the IPs are equal (or boundIp is empty). False = changed.
        bool CheckIpUnchanged(const std::string& boundIp, const std::string& observedIp) const;

    private:
        bool   m_onePerAccount;
        uint32 m_maxAccountsPerIp;

        std::map<uint32, ClientSocket*>          m_accounts;   ///< accountId -> live socket
        std::map<std::string, std::set<uint32> > m_ipAccounts; ///< ip -> distinct accountIds
        mutable ACE_Thread_Mutex                 m_mutex;

    public:
        SessionGuard() : m_onePerAccount(true), m_maxAccountsPerIp(8),
                         m_accounts(), m_ipAccounts(), m_mutex() {}
};

/// Process-wide instance.
SessionGuard& sSessionGuard();

#endif /* GATEWAY_H_SESSIONGUARD */
