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
 * @file test_gateway_protocol.cpp
 * @brief Unit tests for ProtocolValidator (oversize + opcode-in-state).
 */

#include "EdgeChecks.h"
#include "EdgeCheckConfig.h"

#include <cstdio>

static int failures = 0;
static void check(bool c, const char* n)
{ if (c) printf("[PASS] %s\n", n); else { printf("[FAIL] %s\n", n); ++failures; } }

// Opcode numbers (mirror Opcodes.h, kept local to the gateway).
static const uint16 CMSG_CHAR_ENUM     = 0x0037;
static const uint16 CMSG_PLAYER_LOGIN  = 0x003D;
static const uint16 CMSG_MESSAGECHAT   = 0x0095;
static const uint16 CMSG_MOVE_FORWARD  = 0x00B5;

int main(int /*argc*/, char** /*argv*/)
{
    EdgeCheckConfig cfg;
    cfg.maxPayloadBytes = 1024;
    ProtocolValidator v;
    v.Init(cfg);

    // Oversize.
    check(v.Check(CMSG_MOVE_FORWARD, 100, true) == ProtocolValidator::OK, "small payload ok");
    check(v.Check(CMSG_MOVE_FORWARD, 2000, true) == ProtocolValidator::OVERSIZE, "oversize flagged");

    // State: a gameplay opcode (movement) before world entry is illegal.
    check(v.Check(CMSG_MOVE_FORWARD, 50, false) == ProtocolValidator::ILLEGAL_STATE,
          "movement before world -> illegal");
    check(v.Check(CMSG_MOVE_FORWARD, 50, true) == ProtocolValidator::OK,
          "movement after world -> ok");

    // A pre-world allowlisted opcode (char enum) is fine before world entry.
    check(v.Check(CMSG_CHAR_ENUM, 0, false) == ProtocolValidator::OK,
          "char enum pre-world ok");
    // CMSG_PLAYER_LOGIN itself is allowed pre-world (it's the transition).
    check(v.Check(CMSG_PLAYER_LOGIN, 8, false) == ProtocolValidator::OK,
          "player login pre-world ok");
    // Chat before world entry is gameplay -> illegal.
    check(v.Check(CMSG_MESSAGECHAT, 20, false) == ProtocolValidator::ILLEGAL_STATE,
          "chat pre-world -> illegal");

    if (failures == 0) { printf("ALL TESTS PASSED\n"); return 0; }
    printf("%d TEST(S) FAILED\n", failures);
    return 1;
}
