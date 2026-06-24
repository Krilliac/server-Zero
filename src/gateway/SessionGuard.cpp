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
 * @file SessionGuard.cpp
 * @brief SessionGuard implementation (cross-connection account/IP integrity).
 */

#include <ace/Guard_T.h>

#include "SessionGuard.h"

void SessionGuard::Configure(const EdgeCheckConfig& cfg)
{
    ACE_GUARD(ACE_Thread_Mutex, g, m_mutex);
    m_onePerAccount    = cfg.sessionOnePerAccount;
    m_maxAccountsPerIp = cfg.maxAccountsPerIp;
}

SessionGuard::AdmitResult SessionGuard::Register(uint32 accountId, const std::string& ip, ClientSocket* sock)
{
    AdmitResult out;
    ACE_GUARD_RETURN(ACE_Thread_Mutex, g, m_mutex, out);

    // One-live-connection-per-account.
    if (m_onePerAccount)
    {
        std::map<uint32, ClientSocket*>::iterator it = m_accounts.find(accountId);
        if (it != m_accounts.end() && it->second != NULL && it->second != sock)
        {
            out.accountAlreadyLive = true;
            out.previous = it->second;
        }
    }
    m_accounts[accountId] = sock; // newest connection becomes the live one

    // Accounts-per-IP.
    if (!ip.empty())
    {
        std::set<uint32>& accts = m_ipAccounts[ip];
        accts.insert(accountId);
        if (m_maxAccountsPerIp > 0 && (uint32)accts.size() > m_maxAccountsPerIp)
        {
            out.ipCapExceeded = true;
        }
    }

    return out;
}

void SessionGuard::Unregister(uint32 accountId, const std::string& ip, ClientSocket* sock)
{
    ACE_GUARD(ACE_Thread_Mutex, g, m_mutex);

    std::map<uint32, ClientSocket*>::iterator it = m_accounts.find(accountId);
    if (it != m_accounts.end() && it->second == sock)
    {
        m_accounts.erase(it);
    }

    if (!ip.empty())
    {
        std::map<std::string, std::set<uint32> >::iterator ipIt = m_ipAccounts.find(ip);
        if (ipIt != m_ipAccounts.end())
        {
            ipIt->second.erase(accountId);
            if (ipIt->second.empty())
            {
                m_ipAccounts.erase(ipIt);
            }
        }
    }
}

bool SessionGuard::CheckIpUnchanged(const std::string& boundIp, const std::string& observedIp) const
{
    if (boundIp.empty() || observedIp.empty())
    {
        return true; // nothing reliable to compare
    }
    return boundIp == observedIp;
}

SessionGuard& sSessionGuard()
{
    static SessionGuard instance;
    return instance;
}
