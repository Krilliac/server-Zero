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
 * @file test_gateway_rate.cpp
 * @brief Standalone unit tests for the gateway TokenBucket + RateLimiter.
 */

#include "EdgeChecks.h"
#include "EdgeCheckConfig.h"

#include <cstdio>

static int failures = 0;
static void check(bool cond, const char* name)
{
    if (cond) { printf("[PASS] %s\n", name); }
    else      { printf("[FAIL] %s\n", name); ++failures; }
}

int main(int /*argc*/, char** /*argv*/)
{
    // --- TokenBucket: capacity then refill ---
    {
        TokenBucket b;
        b.Init(3, 1.0); // cap 3, 1 token/sec
        // 3 immediate consumes succeed at t=0, the 4th fails.
        check(b.Consume(0, 3, 1.0), "bucket consume 1/3");
        check(b.Consume(0, 3, 1.0), "bucket consume 2/3");
        check(b.Consume(0, 3, 1.0), "bucket consume 3/3");
        check(!b.Consume(0, 3, 1.0), "bucket empty -> reject 4th");
        // After 1s, exactly one token refilled.
        check(b.Consume(1000, 3, 1.0), "bucket refilled 1 token after 1s");
        check(!b.Consume(1000, 3, 1.0), "bucket empty again");
    }

    // --- RateLimiter: per-opcode burst trips RATE_VIOLATION, not disconnect ---
    {
        EdgeCheckConfig cfg;        // defaults: burst 40, refill 20/s, global 300, flood 2000
        cfg.rateBurst = 5;
        cfg.rateRefillPerSec = 0.0; // no refill so the burst is exact
        cfg.globalOpcodesPerSec = 1000000; // don't let the global gate fire here
        cfg.floodDisconnectPerSec = 1000000;
        RateLimiter rl;
        rl.Init(cfg);
        RateLimiter::Result r = RateLimiter::OK;
        for (int i = 0; i < 5; ++i) { r = rl.OnOpcode(0x00B5 /*CMSG_MOVE*/, 0); }
        check(r == RateLimiter::OK, "5 within burst -> OK");
        r = rl.OnOpcode(0x00B5, 0);
        check(r == RateLimiter::RATE_VIOLATION, "6th over burst -> RATE_VIOLATION");
    }

    // --- RateLimiter: global flood -> FLOOD_DISCONNECT ---
    {
        EdgeCheckConfig cfg;
        cfg.rateBurst = 1000000;    // don't let per-opcode fire
        cfg.rateRefillPerSec = 0.0;
        cfg.globalOpcodesPerSec = 50;
        cfg.floodDisconnectPerSec = 100;
        RateLimiter rl;
        rl.Init(cfg);
        RateLimiter::Result r = RateLimiter::OK;
        // 51st packet in the same 1s window crosses globalOpcodesPerSec.
        for (int i = 0; i < 51; ++i) { r = rl.OnOpcode(0x0001, 0); }
        check(r == RateLimiter::RATE_VIOLATION, "51 in 1s -> RATE_VIOLATION");
        // push to 101 -> crosses floodDisconnectPerSec.
        for (int i = 51; i < 101; ++i) { r = rl.OnOpcode(0x0001, 0); }
        check(r == RateLimiter::FLOOD_DISCONNECT, "101 in 1s -> FLOOD_DISCONNECT");
        // new 1s window resets the global counter.
        r = rl.OnOpcode(0x0001, 1000);
        check(r == RateLimiter::OK, "next window -> OK");
    }

    if (failures == 0) { printf("ALL TESTS PASSED\n"); return 0; }
    printf("%d TEST(S) FAILED\n", failures);
    return 1;
}
