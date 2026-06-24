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
 */

/**
 * @file test_migration_validate.cpp
 * @brief Pure-logic tests for the Phase-4 migration travel-budget math used by
 *        Player::ValidateMigrationArrival(). The distance/elapsed gate and its
 *        skip conditions (cross-map, negative elapsed, elapsed > MaxElapsed) are
 *        DB/Player-dependent and exercised live via `.anticheat migtest`; here we
 *        pin the pure budget arithmetic and the skip decisions.
 *
 * The formula is duplicated from AntiCheatMigration.h's MigrationTravelBudget so
 * the test stays standalone (no Common.h / engine deps). Keep in sync with that
 * helper — a comment in both ties them together.
 */

#include <cstdio>
#include <cstdint>

static int failures = 0;
static void check(bool c, const char* n)
{ if (c) printf("[PASS] %s\n", n); else { printf("[FAIL] %s\n", n); ++failures; } }

// Mirror of MigrationTravelBudget(run, tolPct, elapsedSec, teleDist):
//   run * (tolPct/100) * (elapsedSec+1) + teleDist  (run<=0 => 7.0 fallback)
static float travelBudget(float run, uint32_t tolPct, int64_t elapsedSec, uint32_t teleDist)
{
    if (run <= 0.0f) run = 7.0f;
    float tol = float(tolPct) / 100.0f;
    return run * tol * float(elapsedSec + 1) + float(teleDist);
}

// Replicates the SAME gate ValidateMigrationArrival applies before flagging:
// same-map, elapsed in [0, maxElapsed], dist > budget.
static bool wouldFlag(uint32_t lastMap, uint32_t curMap, int64_t elapsedSec,
                      uint32_t maxElapsed, float dist, float budget)
{
    if (lastMap != curMap)            return false;  // cross-map => skip
    if (elapsedSec < 0)               return false;  // clock skew => skip
    if (elapsedSec > int64_t(maxElapsed)) return false; // slow hand-off => skip
    return dist > budget;
}

static void test_budget_value()
{
    // run=7, tolPct=400, elapsed=2s, teleDist=50 => 7*4*3 + 50 = 134
    float b = travelBudget(7.0f, 400, 2, 50);
    check(b > 133.9f && b < 134.1f, "budget(7,400,2,50) == 134");
}

static void test_pass_and_fail()
{
    float b = travelBudget(7.0f, 400, 2, 50);   // 134
    check(!wouldFlag(0, 0, 2, 30, 100.0f, b), "100yd jump PASSES (<=134)");
    check( wouldFlag(0, 0, 2, 30, 300.0f, b), "300yd jump FAILS (>134)");
}

static void test_skip_paths()
{
    float b = travelBudget(7.0f, 400, 2, 50);   // 134
    // cross-map: a 1000yd "jump" must be skipped (portal/teleport is legitimate).
    check(!wouldFlag(0, 1, 2, 30, 1000.0f, b), "cross-map => skip (no flag)");
    // elapsed beyond MaxElapsedSec: bail (slow hand-off / clock skew).
    check(!wouldFlag(0, 0, 40, 30, 1000.0f, b), "elapsed>MaxElapsed => skip");
    // negative elapsed (node clock skew): bail.
    check(!wouldFlag(0, 0, -5, 30, 1000.0f, b), "negative elapsed => skip");
}

static void test_zero_run_fallback()
{
    // run<=0 falls back to 7.0, so a stationary-speed player still gets a budget.
    float b = travelBudget(0.0f, 400, 2, 50);
    check(b > 133.9f && b < 134.1f, "run<=0 falls back to 7.0 baseline");
}

int main(int /*argc*/, char** /*argv*/)
{
    test_budget_value();
    test_pass_and_fail();
    test_skip_paths();
    test_zero_run_fallback();

    if (failures == 0) { printf("ALL TESTS PASSED\n"); return 0; }
    printf("%d TEST(S) FAILED\n", failures);
    return 1;
}
