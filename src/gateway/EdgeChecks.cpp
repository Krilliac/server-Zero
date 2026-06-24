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
 * @file EdgeChecks.cpp
 * @brief Out-of-line members for the gateway edge detectors.
 *
 * TokenBucket and RateLimiter are header-only (small + inlined on the hot path).
 * ProtocolValidator's table-driven members live here.
 */

#include "EdgeChecks.h"

// Opcodes the client legitimately sends after auth but before CMSG_PLAYER_LOGIN.
// Numeric (mirror Opcodes.h); kept local so the gateway needs no game headers.
// char-list/create/delete, name query, ping, realm split, account housekeeping,
// and the login transition itself.
namespace
{
    const uint16 GW_OP_CMSG_CHAR_CREATE          = 0x0036;
    const uint16 GW_OP_CMSG_CHAR_ENUM            = 0x0037;
    const uint16 GW_OP_CMSG_CHAR_DELETE          = 0x0038;
    const uint16 GW_OP_CMSG_PLAYER_LOGIN         = 0x003D;
    const uint16 GW_OP_CMSG_NAME_QUERY           = 0x0050;
    const uint16 GW_OP_CMSG_REALM_SPLIT          = 0x038C;
    const uint16 GW_OP_CMSG_PING                 = 0x01DC;
    const uint16 GW_OP_CMSG_AUTH_SESSION         = 0x01ED;
    const uint16 GW_OP_CMSG_CHAR_RENAME          = 0x02C7;
    const uint16 GW_OP_CMSG_REQUEST_ACCOUNT_DATA = 0x020A;
    const uint16 GW_OP_CMSG_UPDATE_ACCOUNT_DATA  = 0x020B;
}

bool ProtocolValidator::IsPreWorldAllowed(uint16 opcode)
{
    switch (opcode)
    {
        case GW_OP_CMSG_CHAR_CREATE:
        case GW_OP_CMSG_CHAR_ENUM:
        case GW_OP_CMSG_CHAR_DELETE:
        case GW_OP_CMSG_PLAYER_LOGIN:
        case GW_OP_CMSG_NAME_QUERY:
        case GW_OP_CMSG_REALM_SPLIT:
        case GW_OP_CMSG_PING:
        case GW_OP_CMSG_AUTH_SESSION:
        case GW_OP_CMSG_CHAR_RENAME:
        case GW_OP_CMSG_REQUEST_ACCOUNT_DATA:
        case GW_OP_CMSG_UPDATE_ACCOUNT_DATA:
            return true;
        default:
            return false;
    }
}

ProtocolValidator::Result ProtocolValidator::Check(uint16 opcode, uint32 payloadLen, bool worldEntered) const
{
    if (!m_enable)
    {
        return OK;
    }

    if (m_maxPayload > 0 && payloadLen > m_maxPayload)
    {
        return OVERSIZE;
    }

    // The pre-world allowlist is only enforced in strict mode. By default it is
    // OFF: the allowlist cannot be proven exhaustive against a real 1.12 client's
    // char-select traffic, and a missed opcode would both disconnect the player
    // and feed a false positive into the node's ban pipeline. The node already
    // rejects gameplay opcodes before login via its per-opcode STATUS_* gate, so
    // this edge check is opt-in hardening for operators who have validated their
    // client traffic.
    if (m_strictPreWorld && !worldEntered && !IsPreWorldAllowed(opcode))
    {
        return ILLEGAL_STATE;
    }

    return OK;
}
