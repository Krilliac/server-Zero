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
 * @file GatewayAuth.h
 * @brief Pure, testable CMSG_AUTH_SESSION digest verification.
 *
 * This mirrors the SHA-1 challenge/response check that WorldSocket performs
 * in HandleAuthSession. It is deliberately free of any socket, database, or
 * game dependency so it can be unit-tested in isolation: feed it the account
 * name, the two seeds, the session key K and the client-supplied 20-byte
 * digest, and it recomputes the digest and compares.
 */

#ifndef GATEWAY_H_GATEWAYAUTH
#define GATEWAY_H_GATEWAYAUTH

#include "Common.h"

#include <string>

class BigNumber;

namespace GatewayAuth
{
    /**
     * @brief Verify a CMSG_AUTH_SESSION digest against the session key.
     *
     * Recomputes SHA1(account || {0,0,0,0} || clientSeed || serverSeed || K)
     * exactly as WorldSocket::HandleAuthSession does, then compares it to the
     * 20-byte @p clientDigest the client sent.
     *
     * @param account      Account name as sent by the client (used verbatim;
     *                      the bytes must match what the client hashed).
     * @param clientSeed   Random seed chosen by the client.
     * @param serverSeed   Server seed sent in SMSG_AUTH_CHALLENGE.
     * @param K            Session key (from account.sessionkey).
     * @param clientDigest Pointer to the client's 20-byte SHA-1 digest.
     * @return true if the recomputed digest matches; false otherwise.
     */
    bool ValidateDigest(std::string const& account, uint32 clientSeed,
        uint32 serverSeed, BigNumber& K, const uint8* clientDigest);
}

#endif /* GATEWAY_H_GATEWAYAUTH */
