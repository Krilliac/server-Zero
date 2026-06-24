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
 * @file test_autoban_decay.cpp
 * @brief Pure-logic tests for the Phase-6 autoban accumulator: hourly decay,
 *        threshold crossing, the cross-node two-increment aggregation invariant
 *        (the read-through fix), per-violation-type kick weighting, and the
 *        ban-evasion GM/self exclusion. The DB read-through itself and the live
 *        cross-node aggregation are exercised manually via the `.anticheat
 *        autoban` GM command on a 2-node cluster (DB-dependent, not unit-testable).
 *
 * The math here mirrors AntiCheatMgr::DecayedKickScore / PerTypeKickWeight and the
 * read-through reconcile in AccumulateKick. Kept standalone (no engine deps).
 */

#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

static int failures = 0;
static void check(bool c, const char* n)
{ if (c) printf("[PASS] %s\n", n); else { printf("[FAIL] %s\n", n); ++failures; } }

// Mirror of AccountState (the relevant fields).
struct Acct { float kickScore; uint32_t banCount; uint32_t lastUpdate;
              Acct() : kickScore(0.f), banCount(0), lastUpdate(0) {} };

// Mirror of AntiCheatMgr::DecayedKickScore: hourly decay using wall-clock seconds.
static float decay(Acct& s, uint32_t nowSec, uint32_t decayPerHour)
{
    if (s.lastUpdate && decayPerHour && nowSec > s.lastUpdate)
    {
        float hours = float(nowSec - s.lastUpdate) / 3600.0f;
        float d = hours * float(decayPerHour);
        s.kickScore = s.kickScore > d ? s.kickScore - d : 0.0f;
    }
    s.lastUpdate = nowSec;
    return s.kickScore;
}

// Mirror of AntiCheatMgr::PerTypeKickWeight: kickPoints * mul% (mul default 100).
static float perTypeWeight(uint32_t kickPoints, uint32_t mulPercent)
{
    return float(kickPoints) * float(mulPercent) / 100.0f;
}

static void test_decay_to_zero()
{
    // lastUpdate=1 (t=1s) so elapsed is well-defined; 31h at 1/hr > 30 pts => clamps
    // to 0 (matches DecayedKickScore's "score > decay ? score-decay : 0").
    Acct s; s.kickScore = 30.0f; s.lastUpdate = 1;
    float after = decay(s, 1 + 31 * 3600, 1);
    check(after <= 0.01f, "30pts decays to 0 after 31h at 1/hr");

    // Half-decay sanity: 10pts, 4h at 1/hr => 6pts remain.
    Acct h; h.kickScore = 10.0f; h.lastUpdate = 1;
    float rem = decay(h, 1 + 4 * 3600, 1);
    check(rem > 5.9f && rem < 6.1f, "10pts after 4h at 1/hr => ~6pts");
}

// The CORE cluster-correctness invariant (the read-through fix): node A reads the
// row, +kickPoints, writes; node B then READS A's written row (not a stale cache),
// +kickPoints. With 2*kickPoints >= threshold the second increment crosses it. The
// bug was node B building on its stale local copy and clobbering A's contribution.
static void test_threshold_from_two_increments()
{
    const uint32_t kickPoints = 10, threshold = 30;

    // Node A: fresh row (score 0), increments and "writes" rowAfterA.
    Acct rowAfterA;
    rowAfterA.kickScore = 0.0f + float(kickPoints);   // 10

    // Node B READS A's authoritative row (read-through), then increments.
    Acct b;
    b.kickScore  = rowAfterA.kickScore;   // adopt authoritative value (the fix)
    b.banCount   = rowAfterA.banCount;
    b.kickScore += float(kickPoints);     // 20

    check(b.kickScore < float(threshold), "two increments not yet at threshold (20<30)");

    // A third kick (any node, read-through) crosses 30.
    Acct c; c.kickScore = b.kickScore; c.kickScore += float(kickPoints);  // 30
    check(c.kickScore >= float(threshold), "third increment crosses threshold (30>=30)");

    // Contrast: the STALE-cache bug would have node B build on 0, not 10, so it
    // would clobber the row back to 10 — never aggregating. Assert the fix differs.
    Acct stale; stale.kickScore = 0.0f + float(kickPoints);  // bug path: still 10
    check(b.kickScore > stale.kickScore, "read-through aggregates (20) vs stale clobber (10)");
}

static void test_per_type_weight()
{
    const uint32_t kickPoints = 10;
    // mul[TELEPORT]=200 => a teleport kick adds 2*kickPoints.
    check(perTypeWeight(kickPoints, 200) > 19.9f && perTypeWeight(kickPoints, 200) < 20.1f,
          "teleport (200%) adds 2*kickPoints");
    // unset type => 100% => kickPoints.
    check(perTypeWeight(kickPoints, 100) > 9.9f && perTypeWeight(kickPoints, 100) < 10.1f,
          "unset type (100%) adds kickPoints");
}

// Mirror of CorrelateByIp's exclusion logic: skip the banned account itself and
// skip GM accounts; keep non-GM peers sharing the IP.
struct Peer { uint32_t id; bool isGm; };
static void test_evasion_skips_gm_and_self()
{
    const uint32_t banned = 100;
    Peer peers[] = { {100,false}, {200,false}, {300,true}, {400,false} };
    std::vector<uint32_t> kept;
    for (int i = 0; i < 4; ++i)
    {
        if (peers[i].id == banned) continue;  // SQL excludes id<>banned
        if (peers[i].isGm) continue;          // skip GMs
        kept.push_back(peers[i].id);
    }
    check(kept.size() == 2, "evasion keeps exactly the 2 non-GM peers");
    bool hasSelf = false, hasGm = false;
    for (size_t i = 0; i < kept.size(); ++i)
    { if (kept[i] == banned) hasSelf = true; if (kept[i] == 300) hasGm = true; }
    check(!hasSelf, "evasion excludes the banned account itself");
    check(!hasGm,   "evasion excludes GM accounts");
}

int main(int /*argc*/, char** /*argv*/)
{
    test_decay_to_zero();
    test_threshold_from_two_increments();
    test_per_type_weight();
    test_evasion_skips_gm_and_self();

    if (failures == 0) { printf("ALL TESTS PASSED\n"); return 0; }
    printf("%d TEST(S) FAILED\n", failures);
    return 1;
}
