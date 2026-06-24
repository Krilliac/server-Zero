/*
 * Anti-Cheat / Migration seam validation — pure helper (Phase 4, cluster).
 *
 * The migration travel-budget math is extracted here so it can be unit-tested
 * without a live Player/Map. Player::ValidateMigrationArrival() calls this exact
 * helper, so the test covers the real code path.
 */

#ifndef MANGOS_ANTICHEAT_MIGRATION_H
#define MANGOS_ANTICHEAT_MIGRATION_H

#include "Common.h"

// Maximum distance (yd) a migrating player could legitimately have travelled
// across the seam: run-speed * tolerance over (elapsed + 1s hand-off slack), plus
// the single-packet teleport-distance slack (covers a legit blink/charge right at
// the seam). The +1s covers inter-node hand-off latency the shared-DB clock can't
// see. tolPct is a percent (e.g. 400 = 4x run speed).
inline float MigrationTravelBudget(float run, uint32 tolPct, int64 elapsedSec, uint32 teleDist)
{
    if (run <= 0.0f)
        run = 7.0f; // fallback to baseline run speed
    float tol = float(tolPct) / 100.0f;
    return run * tol * float(elapsedSec + 1) + float(teleDist);
}

#endif // MANGOS_ANTICHEAT_MIGRATION_H
