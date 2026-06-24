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
 * @file test_gateway_session.cpp
 * @brief Unit tests for SessionGuard: one-per-account, per-IP cap, IP change.
 */

#include "SessionGuard.h"
#include "EdgeCheckConfig.h"

#include <cstdint>
#include <cstdio>

static int failures = 0;
static void check(bool c, const char* n)
{ if (c) printf("[PASS] %s\n", n); else { printf("[FAIL] %s\n", n); ++failures; } }

// Opaque non-null sentinels; SessionGuard only stores/compares the pointer.
static ClientSocket* S(uintptr_t v) { return reinterpret_cast<ClientSocket*>(v); }

int main(int /*argc*/, char** /*argv*/)
{
    EdgeCheckConfig cfg;
    cfg.maxAccountsPerIp = 2;
    cfg.sessionOnePerAccount = true;
    sSessionGuard().Configure(cfg);

    // First connection for account 100 from 1.1.1.1: clean.
    SessionGuard::AdmitResult r = sSessionGuard().Register(100, "1.1.1.1", S(1));
    check(!r.accountAlreadyLive && !r.ipCapExceeded, "first conn clean");

    // Second connection for the SAME account 100: flagged, previous = S(1).
    r = sSessionGuard().Register(100, "1.1.1.1", S(2));
    check(r.accountAlreadyLive && r.previous == S(1), "duplicate account flagged");

    // IP cap: account 101 from 1.1.1.1 is the 2nd distinct account -> at cap (ok),
    // account 102 from 1.1.1.1 is the 3rd -> exceeds cap of 2.
    r = sSessionGuard().Register(101, "1.1.1.1", S(3));
    check(!r.ipCapExceeded, "2nd account on IP within cap");
    r = sSessionGuard().Register(102, "1.1.1.1", S(4));
    check(r.ipCapExceeded, "3rd account on IP exceeds cap");

    // Unregister account 102 and re-register -> still 3 distinct live accounts.
    sSessionGuard().Unregister(102, "1.1.1.1", S(4));
    r = sSessionGuard().Register(102, "1.1.1.1", S(5));
    check(r.ipCapExceeded, "still 3 distinct accounts -> still over"); // 100,101,102 all live
    sSessionGuard().Unregister(100, "1.1.1.1", S(2)); // S(2) owns acct 100 now
    sSessionGuard().Unregister(101, "1.1.1.1", S(3));
    r = sSessionGuard().Register(103, "1.1.1.1", S(6));
    check(!r.ipCapExceeded, "after frees, new account within cap");

    // IP-change detection.
    check(sSessionGuard().CheckIpUnchanged("9.9.9.9", "9.9.9.9"), "same ip ok");
    check(!sSessionGuard().CheckIpUnchanged("9.9.9.9", "9.9.9.8"), "changed ip detected");
    check(sSessionGuard().CheckIpUnchanged("", "9.9.9.9"), "empty bound ip -> no false alarm");

    if (failures == 0) { printf("ALL TESTS PASSED\n"); return 0; }
    printf("%d TEST(S) FAILED\n", failures);
    return 1;
}
