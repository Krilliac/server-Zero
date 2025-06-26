/**
 * MaNGOS is a full featured server for World of Warcraft, supporting
 * the following clients: 1.12.x, 2.4.3, 3.3.5a, 4.3.4a and 5.4.8
 *
 * Copyright (C) 2005-2025 MaNGOS <https://www.getmangos.eu>
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

#include "MovementChecks.h"
#include "Player.h"
#include "World.h"
#include "AnticheatMgr.h"
#include "Map.h"
#include "TerrainMgr.h"
#include "MovementGenerator.h"
#include "ObjectAccessor.h"
#include "SpellAuras.h"
#include "Log.h"
#include <cmath>
#include <algorithm>
#include <numeric>
#include <sstream>

// ============================================================================
// LOCAL HELPER IMPLEMENTATIONS
// ============================================================================

namespace
{
    /**
     * @class PositionCorrector
     * @brief Blends client and server positions to resolve desyncs
     * 
     * Algorithm:
     *   corrected = α * serverPos + (1 - α) * clientPos
     * Where α is the blend factor (default 0.3)
     */
    class PositionCorrector
    {
    public:
        static Position Reconcile(Player* player, Position serverPos, Position clientPos)
        {
            // Load configurable parameters
            float blendFactor = sConfig.GetFloatDefault("Movement.CorrectionFactor", 0.3f);
            float maxError = sConfig.GetFloatDefault("Movement.MaxPositionError", 0.5f);
            
            float dist = serverPos.GetExactDist(clientPos);
            
            // Apply correction if error exceeds threshold
            if (dist > maxError)
            {
                Position corrected;
                // Linear interpolation formula
                corrected.x = clientPos.x * (1 - blendFactor) + serverPos.x * blendFactor;
                corrected.y = clientPos.y * (1 - blendFactor) + serverPos.y * blendFactor;
                corrected.z = clientPos.z * (1 - blendFactor) + serverPos.z * blendFactor;
                
                // Optional debug logging
                if (sConfig.GetBoolDefault("Movement.LogCorrections", false)) {
                    sLog.outDebug("PositionCorrector: Adjusted %s (%.2fyd error)", 
                                 player->GetName(), dist);
                }
                return corrected;
            }
            return clientPos;
        }
    };

    /**
     * @brief Predicts player position using physics-based simulation
     * 
     * Physics Model:
     *   Horizontal: Δx = v·Δt·cos(θ), Δy = v·Δt·sin(θ)
     *   Vertical: Δz = -½g·t² (when falling)
     * 
     * @param player - Target player
     * @param timeDiff - Time since last update (ms)
     * @return Predicted position
     */
    Position PredictPosition(Player* player, uint32 timeDiff)
    {
        // Get current state
        const Position& currentPos = player->GetPosition();
        float orientation = player->GetOrientation();
        
        // Initialize movement vector components
        float moveX = 0.0f;
        float moveY = 0.0f;
        
        // Calculate movement direction based on flags
        if (player->m_movementInfo.HasMovementFlag(MOVEFLAG_FORWARD)) {
            moveX += std::cos(orientation);
            moveY += std::sin(orientation);
        }
        if (player->m_movementInfo.HasMovementFlag(MOVEFLAG_BACKWARD)) {
            moveX -= std::cos(orientation);
            moveY -= std::sin(orientation);
        }
        if (player->m_movementInfo.HasMovementFlag(MOVEFLAG_STRAFE_LEFT)) {
            moveX += std::cos(orientation + M_PI/2);
            moveY += std::sin(orientation + M_PI/2);
        }
        if (player->m_movementInfo.HasMovementFlag(MOVEFLAG_STRAFE_RIGHT)) {
            moveX += std::cos(orientation - M_PI/2);
            moveY += std::sin(orientation - M_PI/2);
        }
        
        // Normalize movement vector to prevent diagonal speed boost
        float moveLength = std::sqrt(moveX*moveX + moveY*moveY);
        if (moveLength > 0.001f) {
            moveX /= moveLength;
            moveY /= moveLength;
        }
        
        // Calculate movement distance: d = v·t
        float speed = player->GetSpeed(MOVE_RUN);
        float distance = speed * (timeDiff / 1000.0f);  // Convert ms to seconds
        
        // Apply horizontal movement
        Position predicted;
        predicted.x = currentPos.x + moveX * distance;
        predicted.y = currentPos.y + moveY * distance;
        predicted.z = currentPos.z;  // Z handled separately
        
        // Apply gravity if falling: Δz = -½gt²
        if (player->m_movementInfo.HasMovementFlag(MOVEFLAG_FALLING)) {
            float fallTime = timeDiff / 1000.0f;  // Convert to seconds
            predicted.z -= 0.5f * MovementChecks::GRAVITY * fallTime * fallTime;
        }
        
        return predicted;
    }
}

// ============================================================================
// CORE MOVEMENT VALIDATION
// ============================================================================

/*static*/ void MovementChecks::FullMovementCheck(Player* player, const MovementInfo& moveInfo)
{
    /**
     * Main entry point for comprehensive movement validation
     * 
     * Processing Phases:
     * 1. Position Reconciliation: Corrects client/server desyncs
     * 2. Core Validation: Physics, state, speed, and environment checks
     * 3. Bot Detection: Periodic analysis of movement patterns
     * 
     * Skip checks for GMs and during teleportation
     */
    if (player->IsGameMaster() || player->IsBeingTeleported()) {
        return;
    }
    
    // --- PHASE 1: POSITION RECONCILIATION ---
    uint32 timeDiff = moveInfo.GetTime() - player->GetLastMoveTime();
    Position predictedPos = PredictPosition(player, timeDiff);
    Position correctedPos = PositionCorrector::Reconcile(player, predictedPos, moveInfo.GetPos());
    
    // Apply correction if significant difference
    if (correctedPos.GetExactDist(moveInfo.GetPos()) > 0.01f) {
        player->SetPosition(correctedPos);
        // Update movement info for consistent validation
        const_cast<MovementInfo&>(moveInfo).ChangePosition(correctedPos);
    }
    
    // --- PHASE 2: CORE VALIDATION ---
    ValidatePhysics(player, moveInfo);         // Physics-based checks
    ValidateStateTransitions(player, moveInfo); // State change validation
    CheckTeleport(player, moveInfo);            // Teleport detection
    CheckSpeed(player, moveInfo);               // Speed validation
    ValidateEnvironment(player, moveInfo.GetPos()); // Environmental checks
    ValidateSplineMovement(player, moveInfo);   // Spline movement validation
    ValidateTimestamps(player, moveInfo);       // Time synchronization
    
    // --- PHASE 3: BOT DETECTION ---
    DetectBotPatterns(player, moveInfo);       // Bot movement analysis
}

// ================= PHYSICS VALIDATION ================= //

/*static*/ void MovementChecks::ValidatePhysics(Player* player, const MovementInfo& moveInfo)
{
    /**
     * Validates physical movement properties:
     * - Falling physics
     * - Vertical movement (fly hacks)
     * - Collision detection
     */
    const Position& newPos = moveInfo.GetPos();
    const Position& prevPos = player->GetPosition();
    
    // Validate falling physics if applicable
    if (moveInfo.HasMovementFlag(MOVEFLAG_FALLING)) {
        ValidateFallPhysics(player, moveInfo);
    }
    
    // Verify vertical movement
    ValidateVerticalMovement(player, newPos);
    
    // Check for collision bypass
    ValidateCollision(player, prevPos, newPos);
}

/*static*/ void MovementChecks::ValidateFallPhysics(Player* player, const MovementInfo& moveInfo)
{
    /**
     * Validates fall time using free-fall equation:
     *   t = √(2h/g)
     * Where:
     *   t = expected fall time (seconds)
     *   h = fall distance
     *   g = gravity constant
     */
    uint32 fallTime = moveInfo.GetFallTime();
    float fallDistance = moveInfo.GetFallStart() - moveInfo.GetPos().z;
    
    // Calculate expected fall time
    float expectedTime = sqrtf(2 * fallDistance / GRAVITY) * 1000;  // Convert to ms
    
    // Compare with tolerance
    if (abs(static_cast<int32>(fallTime - expectedTime)) > FALL_TIME_TOLERANCE) {
        sAnticheatMgr->RecordViolation(player, CHEAT_TIME_DESYNC, 
            fmt::format("Fall time: {}ms vs expected {:.1f}ms", fallTime, expectedTime));
    }
}

/*static*/ void MovementChecks::ValidateVerticalMovement(Player* player, const Position& newPos)
{
    /**
     * Validates Z-axis position against terrain:
     * 1. Fly hack detection: Checks for illegal height above terrain
     * 2. Water walking: Verifies proper liquid interaction
     */
    float terrainHeight = player->GetMap()->GetHeight(newPos.x, newPos.y, MAX_HEIGHT);
    float liquidLevel = player->GetMap()->GetWaterLevel(newPos.x, newPos.y);
    float zDelta = newPos.z - terrainHeight;
    
    // Fly hack detection
    if (zDelta > MAX_Z_DELTA && !player->IsFlying() && !player->IsFalling()) {
        sAnticheatMgr->RecordViolation(player, CHEAT_FLY_HACK,
            fmt::format("Z-delta: {:.2f}", zDelta));
    }
    
    // Water walking validation
    if (player->IsInWater() && newPos.z > liquidLevel + WATER_WALK_HEIGHT) {
        if (!player->HasAuraType(SPELL_AURA_WATER_WALK)) {
            sAnticheatMgr->RecordViolation(player, CHEAT_WATER_WALK,
                fmt::format("Water level: {:.2f} Position: {:.2f}", liquidLevel, newPos.z));
        }
    }
}

/*static*/ void MovementChecks::ValidateCollision(Player* player, const Position& from, const Position& to)
{
    /**
     * Uses raycasting to detect collision bypass:
     *   R(t) = start + t·(end - start), 0 ≤ t ≤ 1
     * Records violation if player bypasses significant collision
     * 
     * Skip collision checks in instances
     */
    if (player->GetMap()->Instanceable()) {
        return;
    }
        
    Vector3 start(from.x, from.y, from.z);
    Vector3 end(to.x, to.y, to.z);
    Vector3 hitPos;
    float hitDist;
    
    // Perform raycast collision check
    if (player->GetMap()->GetHitPosition(start, end, hitPos, hitDist, COLLISION_TOLERANCE)) {
        float bypassDistance = start.distance(end);
        float actualDistance = start.distance(hitPos);
        
        // Allow small tolerance for uneven terrain
        if (bypassDistance - actualDistance > COLLISION_TOLERANCE * 2) {
            sAnticheatMgr->RecordViolation(player, CHEAT_WALL_CLIMB,
                fmt::format("Bypassed {:.2f} units of collision", bypassDistance - actualDistance));
        }
    }
}

// ================= STATE TRANSITIONS ================= //

/*static*/ void MovementChecks::ValidateStateTransitions(Player* player, const MovementInfo& moveInfo)
{
    /**
     * Validates physical state transitions:
     * 1. Swimming: Entering water without swim ability
     * 2. Flying: Flying without proper ability
     */
    const Position& newPos = moveInfo.GetPos();
    float distance = player->GetPosition().GetExactDist(newPos);
    
    // Swimming state transition
    bool wasSwimming = player->IsInWater();
    bool isSwimming = player->GetMap()->IsInWater(newPos.x, newPos.y, newPos.z);
    
    if (isSwimming != wasSwimming && distance > 5.0f) {
        if (!wasSwimming && !player->CanSwim()) {
            sAnticheatMgr->RecordViolation(player, CHEAT_WATER_WALK,
                "Entered water without swimming ability");
        }
    }
    
    // Flying state validation
    if (moveInfo.HasMovementFlag(MOVEFLAG_FLYING) && !player->CanFly()) {
        sAnticheatMgr->RecordViolation(player, CHEAT_FLY_HACK,
            "Flying without ability");
    }
}

// ================= ANTI-CHEAT DETECTION ================= //

/*static*/ void MovementChecks::CheckTeleport(Player* player, const MovementInfo& moveInfo)
{
    /**
     * Detects teleportation by comparing distance traveled vs maximum possible:
     *   dₘₐₓ = v·Δt·multiplier
     * Uses multiplier for leniency (default 1.8)
     */
    const Position& prevPos = player->GetPosition();
    const Position& newPos = moveInfo.GetPos();
    float distance = prevPos.GetExactDist(newPos);
    
    // Calculate maximum possible distance
    uint32 timeDiff = moveInfo.GetTime() - player->GetLastMoveTime();
    float maxPossible = player->GetSpeed(MOVE_RUN) * (timeDiff / 1000.0f) * TELEPORT_MULTIPLIER;
    
    // Account for legitimate teleports
    bool isTeleporting = player->IsTeleporting() || 
                         player->GetMotionMaster()->GetCurrentMovementGeneratorType() == TELEPORT_MOTION_TYPE;
    
    if (distance > maxPossible && !isTeleporting) {
        sAnticheatMgr->RecordViolation(player, CHEAT_TELEPORT, 
            fmt::format("Distance: {:.2f}yd > Max: {:.2f}yd in {}ms", distance, maxPossible, timeDiff));
    }
}

/*static*/ void MovementChecks::CheckSpeed(Player* player, const MovementInfo& moveInfo)
{
    /**
     * Validates movement speed using:
     *   v_actual = distance / Δt
     * Compares against expected speed with dynamic tolerance
     */
    const Position& prevPos = player->GetPosition();
    const Position& newPos = moveInfo.GetPos();
    float distance = prevPos.GetExactDist(newPos);
    uint32 timeDiff = moveInfo.GetTime() - player->GetLastMoveTime();
    
    if (timeDiff == 0) return;  // Prevent division by zero
    
    // Calculate actual speed
    float actualSpeed = distance / (timeDiff / 1000.0f);
    
    // Get expected speed based on movement mode
    float expectedSpeed = player->GetSpeed(SelectSpeedType(moveInfo));
    
    // Apply dynamic tolerance
    float tolerance = GetSpeedTolerance(player, moveInfo);
    
    if (actualSpeed > expectedSpeed * tolerance) {
        sAnticheatMgr->RecordViolation(player, CHEAT_SPEED_HACK, 
            fmt::format("Speed: {:.1f}yd/s > {:.1f}yd/s (Tolerance: {:.2f})", 
            actualSpeed, expectedSpeed, tolerance));
    }
}

/*static*/ float MovementChecks::GetSpeedTolerance(Player* player, const MovementInfo& moveInfo)
{
    /**
     * Calculates dynamic speed tolerance based on environment:
     * - Increases tolerance in water, flying, or mounted
     * - Decreases tolerance on roads
     */
    float tolerance = SPEED_TOLERANCE;
    
    // Environment-based adjustments
    if (player->IsInWater()) tolerance *= 1.3f;
    if (player->IsFlying()) tolerance *= 1.5f;
    if (player->IsMounted()) tolerance *= 1.4f;
    if (player->GetMap()->IsOnRoad(player->GetPositionX(), player->GetPositionY())) {
        tolerance *= 0.9f;
    }
        
    return tolerance;
}

/*static*/ UnitMoveType MovementChecks::SelectSpeedType(const MovementInfo& moveInfo)
{
    /**
     * Determines movement mode for speed calculation
     */
    if (moveInfo.HasMovementFlag(MOVEFLAG_SWIMMING)) return MOVE_SWIM;
    if (moveInfo.HasMovementFlag(MOVEFLAG_FLYING)) return MOVE_FLIGHT;
    if (moveInfo.HasMovementFlag(MOVEFLAG_WALK_MODE)) return MOVE_WALK;
    return MOVE_RUN;
}

// ================= ENVIRONMENTAL VALIDATION ================= //

/*static*/ void MovementChecks::ValidateEnvironment(Player* player, const Position& newPos)
{
    /**
     * Validates environmental interactions:
     * 1. Water walking ability
     * 2. Hover ability
     * 3. Transport states
     */
    // Water walking validation
    if (player->m_movementInfo.HasMovementFlag(MOVEFLAG_WATERWALKING) && 
        !player->CanWaterWalk() && 
        !player->HasAuraType(SPELL_AURA_WATER_WALK)) {
        sAnticheatMgr->RecordViolation(player, CHEAT_WATER_WALK, "Invalid water walking");
    }

    // Hover validation
    if (player->m_movementInfo.HasMovementFlag(MOVEFLAG_HOVER) && 
        !player->CanHover() && 
        !player->HasAuraType(SPELL_AURA_HOVER)) {
        sAnticheatMgr->RecordViolation(player, CHEAT_HOVER, "Invalid hover");
    }
    
    // Transport validation
    if (player->m_movementInfo.HasMovementFlag(MOVEFLAG_ONTRANSPORT) && !player->GetTransport()) {
        sAnticheatMgr->RecordViolation(player, CHEAT_TRANSPORT, "Invalid transport flag");
    }
}

// ================= SPLINE VALIDATION ================= //

/*static*/ void MovementChecks::ValidateSplineMovement(Player* player, const MovementInfo& moveInfo)
{
    /**
     * Validates spline-based movement:
     * 1. Elevation accuracy
     * 2. Speed limits
     */
    if (!player->m_movementInfo.HasMovementFlag(MOVEFLAG_SPLINE_ENABLED)) {
        return;
    }
        
    float splineHeight = player->m_movementInfo.GetSplineElevation();
    float actualHeight = moveInfo.GetPos().z - player->GetPosition().z;
    float heightDiff = fabs(splineHeight - actualHeight);
    
    if (heightDiff > 0.5f) {
        sAnticheatMgr->RecordViolation(player, CHEAT_SPLINE_HEIGHT, 
            fmt::format("Spline height: {:.2f} vs actual {:.2f}", splineHeight, actualHeight));
    }
    
    // Validate spline movement speed
    if (player->m_movementInfo.HasMovementFlag(MOVEFLAG_SPLINE_ENABLED)) {
        float splineSpeed = player->m_movementInfo.GetSplineSpeed();
        float allowedSpeed = player->GetSpeed(MOVE_RUN) * 1.8f;
        
        if (splineSpeed > allowedSpeed) {
            sAnticheatMgr->RecordViolation(player, CHEAT_SPLINE_SPEED, 
                fmt::format("Spline speed: {:.1f}yd/s > max {:.1f}yd/s", splineSpeed, allowedSpeed));
        }
    }
}

// ================= TIME SYNCHRONIZATION ================= //

/*static*/ void MovementChecks::ValidateTimestamps(Player* player, const MovementInfo& moveInfo)
{
    /**
     * Validates movement timestamps:
     * 1. Packet time vs server time
     * 2. Fall duration consistency
     */
    uint32 serverTime = WorldTimer::getMSTime();
    uint32 packetTime = moveInfo.GetTime();
    uint32 latency = player->GetSession()->GetLatency();
    
    // Calculate time difference with latency compensation
    int32 timeDifference = abs(static_cast<int32>(packetTime - serverTime) - 
                              static_cast<int32>(latency));
    
    if (timeDifference > FALL_TIME_TOLERANCE) {
        sAnticheatMgr->RecordViolation(player, CHEAT_TIME_DESYNC, 
            fmt::format("Time diff: {}ms > allowed {}ms (Latency: {}ms)", 
            timeDifference, FALL_TIME_TOLERANCE, latency));
    }
    
    // Validate fall start time consistency
    if (moveInfo.HasMovementFlag(MOVEFLAG_FALLING)) {
        uint32 fallStartDiff = moveInfo.GetFallTime() - moveInfo.GetFallStartTime();
        if (fallStartDiff > 10000) { // 10 seconds max fall
            sAnticheatMgr->RecordViolation(player, CHEAT_TIME_DESYNC,
                fmt::format("Fall duration too long: {}ms", fallStartDiff));
        }
    }
}

// ================= BOT DETECTION SYSTEM ================= //

/*static*/ void MovementChecks::DetectBotPatterns(Player* player, const MovementInfo& moveInfo)
{
    /**
     * Central bot detection coordinator:
     * 1. Runs periodic analysis (every 5 minutes)
     * 2. Orchestrates various detection methods
     */
    static std::map<ObjectGuid, uint32> lastAnalysisTime;
    uint32 now = WorldTimer::getMSTime();
    uint32 lastCheck = lastAnalysisTime[player->GetGUID()];
    
    // Run analysis every 5 minutes
    if (lastCheck > 0 && (now - lastCheck) < BOT_ANALYSIS_INTERVAL) {
        return;
    }
    
    lastAnalysisTime[player->GetGUID()] = now;
    
    // Execute all bot detection methods
    AnalyzeMovementRepetition(player);        // Path repetition analysis
    DetectMovementSharpness(player);          // Movement sharpness detection
    AnalyzeMovementRandomness(player);        // Movement randomness analysis
}

/*static*/ void MovementChecks::AnalyzeMovementRepetition(Player* player)
{
    /**
     * Detects botting patterns through path repetition analysis:
     * 1. Simplifies movement path using Douglas-Peucker algorithm
     * 2. Discretizes waypoints into grid cells
     * 3. Computes Longest Common Prefix (LCP) metric
     * 4. Flags players with high path repetition
     */
    std::vector<Position> waypoints;
    BuildWaypointSequence(player, waypoints);
    
    // Require sufficient data for analysis
    if (waypoints.size() < MIN_WAYPOINTS_FOR_ANALYSIS) {
        return;
    }
    
    // Discretize waypoints into grid cells
    std::vector<int> sequence;
    for (const Position& wp : waypoints) {
        int gridX = static_cast<int>(wp.x / GRID_SIZE);
        int gridY = static_cast<int>(wp.y / GRID_SIZE);
        sequence.push_back(gridX * 10000 + gridY); // Unique cell ID
    }
    
    // Calculate average Longest Common Prefix
    double avgLCP = CalculateAverageLCP(sequence);
    
    // Threshold-based detection
    if (avgLCP > BOT_LCP_THRESHOLD) {
        sAnticheatMgr->RecordViolation(player, CHEAT_BOT, 
            fmt::format("Path repetition detected: LCP={:.2f}", avgLCP));
    }
}

/*static*/ void MovementChecks::BuildWaypointSequence(Player* player, std::vector<Position>& waypoints)
{
    /**
     * Applies Douglas-Peucker path simplification:
     * 1. Start with direct path between endpoints
     * 2. Find point with maximum deviation
     * 3. If deviation > threshold, split path
     * 4. Recursively process segments
     */
    const auto& history = player->GetMovementHistory();
    if (history.size() < 2) return;
    
    // Initialize markers (first and last points always kept)
    std::vector<bool> markers(history.size(), false);
    markers[0] = true;
    markers[history.size()-1] = true;
    
    // Apply Douglas-Peucker simplification with 2-yard threshold
    SimplifyPath(history, 0, history.size()-1, 2.0f, markers);
    
    // Extract marked waypoints
    for (size_t i = 0; i < history.size(); ++i) {
        if (markers[i]) {
            waypoints.push_back(history[i].pos);
        }
    }
}

/*static*/ void MovementChecks::SimplifyPath(const std::vector<MovementRecord>& points, 
                                             size_t start, size_t end, 
                                             float epsilon, std::vector<bool>& markers)
{
    /**
     * Recursive Douglas-Peucker implementation:
     * 1. Find point with max perpendicular distance
     * 2. If distance > epsilon, mark point and recurse
     */
    // Find point with maximum distance
    float maxDist = 0;
    size_t index = start;
    
    for (size_t i = start + 1; i < end; ++i) {
        float dist = PerpendicularDistance(points[i].pos, points[start].pos, points[end].pos);
        if (dist > maxDist) {
            index = i;
            maxDist = dist;
        }
    }
    
    // If max distance > epsilon, recursively simplify
    if (maxDist > epsilon) {
        markers[index] = true;
        SimplifyPath(points, start, index, epsilon, markers);
        SimplifyPath(points, index, end, epsilon, markers);
    }
}

/*static*/ float MovementChecks::PerpendicularDistance(const Position& point, 
                                                      const Position& lineStart, 
                                                      const Position& lineEnd)
{
    /**
     * Calculates perpendicular distance from point to line segment
     * 
     * Formula:
     *   d = |(x2-x1)(y1-y0) - (x1-x0)(y2-y1)| / √((x2-x1)² + (y2-y1)²)
     */
    float dx = lineEnd.x - lineStart.x;
    float dy = lineEnd.y - lineStart.y;
    
    // If line is a point, return distance to that point
    float lengthSq = dx*dx + dy*dy;
    if (lengthSq < 1e-6f) {
        return point.GetExactDist(lineStart);
    }
    
    // Calculate projection factor
    float t = ((point.x - lineStart.x)*dx + (point.y - lineStart.y)*dy) / lengthSq;
    t = std::clamp(t, 0.0f, 1.0f); // Clamp to line segment
    
    // Calculate projection point
    float projX = lineStart.x + t * dx;
    float projY = lineStart.y + t * dy;
    
    // Return distance to projection point
    return point.GetExactDist2d(projX, projY);
}

/*static*/ double MovementChecks::CalculateAverageLCP(const std::vector<int>& sequence)
{
    /**
     * Computes average Longest Common Prefix (LCP):
     * 1. Build suffix array
     * 2. Sort suffixes lexicographically
     * 3. Compute LCP between consecutive suffixes
     * 4. Return average LCP
     */
    if (sequence.size() < 2) {
        return 0.0;
    }
    
    // Build suffix array
    std::vector<size_t> suffix(sequence.size());
    std::iota(suffix.begin(), suffix.end(), 0);
    
    // Sort suffixes lexicographically
    std::sort(suffix.begin(), suffix.end(), [&](size_t a, size_t b) {
        return std::lexicographical_compare(
            sequence.begin() + a, sequence.end(),
            sequence.begin() + b, sequence.end()
        );
    });
    
    // Compute LCP array
    std::vector<size_t> lcp(sequence.size(), 0);
    for (size_t i = 1; i < sequence.size(); ++i) {
        size_t a = suffix[i-1];
        size_t b = suffix[i];
        size_t len = 0;
        
        // Find common prefix length
        while (a + len < sequence.size() && 
               b + len < sequence.size() && 
               sequence[a + len] == sequence[b + len]) {
            len++;
        }
        lcp[i] = len;
    }
    
    // Calculate average LCP (skip first element which is always 0)
    double total = 0.0;
    size_t count = 0;
    for (size_t i = 1; i < lcp.size(); ++i) {
        total += lcp[i];
        count++;
    }
    
    return count > 0 ? total / count : 0.0;
}

/*static*/ void MovementChecks::DetectMovementSharpness(Player* player)
{
    /**
     * Detects unnaturally sharp movement angles characteristic of bots:
     * 1. Calculates angle between consecutive movement vectors
     * 2. Averages angles over recent movement history
     * 3. Flags if average angle is below threshold (11.5°)
     */
    const auto& history = player->GetMovementHistory();
    if (history.size() < 3) return; // Need at least 3 points

    float totalAngleChange = 0.0f;
    int count = 0;

    for (size_t i = 2; i < history.size(); ++i) {
        Vector3 v1 = history[i-1].pos - history[i-2].pos;
        Vector3 v2 = history[i].pos - history[i-1].pos;

        // Ignore negligible movement
        if (v1.length() < 0.1f || v2.length() < 0.1f) continue;

        // Normalize vectors
        v1 = v1.normalized();
        v2 = v2.normalized();

        // Calculate angle between vectors: θ = arccos(v1·v2)
        float dot = std::clamp(v1.dot(v2), -1.0f, 1.0f);
        float angle = std::acos(dot); // Angle in radians
        totalAngleChange += angle;
        ++count;
    }

    if (count == 0) return;

    float avgAngleChange = totalAngleChange / count;

    // Threshold for sharpness (11.5 degrees)
    if (avgAngleChange < SHARPNESS_THRESHOLD) {
        sAnticheatMgr->RecordViolation(player, CHEAT_BOT, 
            fmt::format("Sharp movement: avg angle={:.2f} rad", avgAngleChange));
    }
}

/*static*/ void MovementChecks::AnalyzeMovementRandomness(Player* player)
{
    /**
     * Detects bot-like predictable movement through:
     * 1. Angle variance calculation
     * 2. Low variance indicates mechanical movement
     */
    const auto& history = player->GetMovementHistory();
    if (history.size() < 3) return;

    std::vector<float> angles;

    for (size_t i = 2; i < history.size(); ++i) {
        Vector3 v1 = history[i-1].pos - history[i-2].pos;
        Vector3 v2 = history[i].pos - history[i-1].pos;

        // Skip small movements
        if (v1.length() < 0.1f || v2.length() < 0.1f) continue;

        // Normalize vectors
        v1 = v1.normalized();
        v2 = v2.normalized();

        // Calculate angle: θ = arccos(v1·v2)
        float dot = std::clamp(v1.dot(v2), -1.0f, 1.0f);
        float angle = std::acos(dot);
        angles.push_back(angle);
    }

    // Require sufficient data
    if (angles.size() < 10) return;

    // Calculate mean angle
    float mean = std::accumulate(angles.begin(), angles.end(), 0.0f) / angles.size();
    
    // Calculate variance: σ² = Σ(θ_i - μ)² / N
    float variance = 0.0f;
    for (float a : angles) {
        variance += (a - mean) * (a - mean);
    }
    variance /= angles.size();

    // Threshold for low randomness
    if (variance < RANDOMNESS_THRESHOLD) {
        sAnticheatMgr->RecordViolation(player, CHEAT_BOT, 
            fmt::format("Low randomness: variance={:.4f}", variance));
    }
}