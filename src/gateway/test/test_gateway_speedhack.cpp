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
 * @file test_gateway_speedhack.cpp
 * @brief Unit tests for SpeedHackDetector over synthetic delta sequences.
 */

#include "SpeedHackDetector.h"

#include <cstdio>

static int failures = 0;
static void check(bool c, const char* n)
{ if (c) printf("[PASS] %s\n", n); else { printf("[FAIL] %s\n", n); ++failures; } }

static SpeedHackConfig defaultCfg()
{
    SpeedHackConfig c;
    c.enable = true;
    c.window = 20;
    c.minSamples = 12;
    c.tolerancePct = 30;
    c.sustainWindows = 3;
    c.maxGapMs = 3000;
    c.cooldownMs = 10000;
    return c;
}

// --- test_normal_play_passes: heartbeat cadence with real-side jitter ---
static void test_normal_play_passes()
{
    SpeedHackConfig cfg = defaultCfg();
    SpeedHackDetector d(cfg);

    uint32 ct = 100000, rt = 5000;
    bool fired = false;
    // deterministic pseudo-jitter on the REAL side (lag inflates rd -> ratio<1)
    const int jit[8] = { +60, -40, +80, -20, +30, -70, +50, -10 };
    for (int i = 0; i < 40; ++i)
    {
        ct += 500;
        rt += 500 + jit[i % 8];
        SpeedHackDecision dec = d.Feed(ct, rt);
        if (dec.fire) { fired = true; }
    }
    check(!fired, "normal play (heartbeat + jitter) does not fire");
}

// --- test_accelerated_clock_trips: client clock 1.5x real ---
static void test_accelerated_clock_trips()
{
    SpeedHackConfig cfg = defaultCfg();
    SpeedHackDetector d(cfg);

    uint32 ct = 200000, rt = 7000;
    bool fired = false;
    uint8 sev = 0;
    double ratio = 0.0;
    const int steps = cfg.minSamples + cfg.sustainWindows * cfg.window + 5;
    for (int i = 0; i < steps; ++i)
    {
        rt += 500;
        ct += 750; // 1.5x faster
        SpeedHackDecision dec = d.Feed(ct, rt);
        if (dec.fire) { fired = true; sev = dec.severity; ratio = dec.ratio; }
    }
    check(fired, "accelerated 1.5x clock fires");
    check(sev >= 45 && sev <= 55, "severity ~50 for 1.5x");
    check(ratio > 1.40 && ratio < 1.60, "ratio ~1.5");
}

// --- test_lag_does_not_trip: real deltas 2x client (heavy lag) ---
static void test_lag_does_not_trip()
{
    SpeedHackConfig cfg = defaultCfg();
    SpeedHackDetector d(cfg);

    uint32 ct = 300000, rt = 9000;
    bool fired = false;
    for (int i = 0; i < 60; ++i)
    {
        ct += 500;
        rt += 1000; // ratio ~0.5
        SpeedHackDecision dec = d.Feed(ct, rt);
        if (dec.fire) { fired = true; }
    }
    check(!fired, "heavy lag (ratio 0.5) does not fire");
}

// --- test_idle_gap_filtered: one 60s AFK pair is discarded ---
static void test_idle_gap_filtered()
{
    SpeedHackConfig cfg = defaultCfg();
    SpeedHackDetector d(cfg);

    uint32 ct = 400000, rt = 11000;
    bool fired = false;

    // a few normal pairs
    for (int i = 0; i < 6; ++i)
    {
        ct += 500; rt += 500;
        SpeedHackDecision dec = d.Feed(ct, rt);
        if (dec.fire) { fired = true; }
    }
    // one giant idle gap with stale clientTime (rd > maxGapMs -> discarded)
    ct += 200;       // client barely advanced (AFK, no movement reported)
    rt += 60000;     // 1 minute real
    SpeedHackDecision dec = d.Feed(ct, rt);
    if (dec.fire) { fired = true; }

    // resume normal play
    for (int i = 0; i < 30; ++i)
    {
        ct += 500; rt += 500;
        SpeedHackDecision dd = d.Feed(ct, rt);
        if (dd.fire) { fired = true; }
    }
    check(!fired, "idle gap is filtered and normal play resumes clean");
}

// --- test_borderline_hysteresis: oscillation needs sustain to fire ---
static void test_borderline_hysteresis()
{
    SpeedHackConfig cfg = defaultCfg();
    SpeedHackDetector d(cfg);

    uint32 ct = 500000, rt = 13000;
    bool firedEarly = false;

    // Build a baseline well inside the hysteresis band but below tripHigh so no
    // single full window starts hot. ratio target ~1.20 (between 1.15 and 1.30).
    for (int i = 0; i < cfg.window; ++i)
    {
        rt += 500;
        ct += 600; // ratio 1.2
        SpeedHackDecision dec = d.Feed(ct, rt);
        if (dec.fire) { firedEarly = true; }
    }
    check(!firedEarly, "ratio in hysteresis band (1.2) does not fire");

    // Now push the window hot (>=1.30) for fewer than sustainWindows evals: it
    // must NOT fire until sustainWindows consecutive hot windows accrue.
    // One hot Feed only nudges the windowed ratio; verify a clearly-hot sustained
    // run eventually fires (sanity that the sustain gate is the only thing holding).
    bool firedLate = false;
    for (int i = 0; i < cfg.window * cfg.sustainWindows + cfg.window; ++i)
    {
        rt += 500;
        ct += 800; // ratio 1.6 -> drives window hot
        SpeedHackDecision dec = d.Feed(ct, rt);
        if (dec.fire) { firedLate = true; }
    }
    check(firedLate, "sustained hot windows eventually fire");
}

// --- test_cooldown: no second fire within cooldown, fire again after ---
static void test_cooldown()
{
    SpeedHackConfig cfg = defaultCfg();
    SpeedHackDetector d(cfg);

    uint32 ct = 600000, rt = 15000;
    int fires = 0;

    // Drive a clear 1.5x hack until the first fire.
    bool firstFired = false;
    while (!firstFired)
    {
        rt += 500; ct += 750;
        SpeedHackDecision dec = d.Feed(ct, rt);
        if (dec.fire) { firstFired = true; ++fires; }
        if (rt > 15000 + 200000) { break; } // safety
    }
    check(firstFired, "first fire occurs");

    // Keep feeding hot samples WITHIN the cooldown window: no second fire.
    int firesDuringCooldown = 0;
    uint32 cooldownStart = rt;
    while (rt - cooldownStart < cfg.cooldownMs - 1000)
    {
        rt += 500; ct += 750;
        SpeedHackDecision dec = d.Feed(ct, rt);
        if (dec.fire) { ++firesDuringCooldown; }
    }
    check(firesDuringCooldown == 0, "no second fire during cooldown");

    // Advance past the cooldown and keep feeding hot: it can fire again.
    bool refired = false;
    for (int i = 0; i < cfg.window * (cfg.sustainWindows + 1) + 4; ++i)
    {
        rt += 500; ct += 750;
        SpeedHackDecision dec = d.Feed(ct, rt);
        if (dec.fire) { refired = true; }
    }
    check(refired, "can fire again after cooldown elapses");
}

int main(int /*argc*/, char** /*argv*/)
{
    test_normal_play_passes();
    test_accelerated_clock_trips();
    test_lag_does_not_trip();
    test_idle_gap_filtered();
    test_borderline_hysteresis();
    test_cooldown();

    if (failures == 0) { printf("ALL TESTS PASSED\n"); return 0; }
    printf("%d TEST(S) FAILED\n", failures);
    return 1;
}
