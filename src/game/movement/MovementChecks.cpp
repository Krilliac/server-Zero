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

void MovementChecks::FullMovementCheck(Player* player, const MovementInfo& moveInfo)
{
    // Skip checks for GMs and during teleportation
    if (player->IsGameMaster() || player->IsBeingTeleported())
        return;
    
    // 1. Physics-based checks
    ValidatePhysics(player, moveInfo);
    
    // 2. State-specific checks
    ValidateStateTransitions(player, moveInfo);
    
    // 3. Advanced teleport detection
    CheckTeleport(player, moveInfo);
    
    // 4. Precision speed validation
    CheckSpeed(player, moveInfo);
    
    // 5. Environmental validation
    ValidateEnvironment(player, moveInfo.GetPos());
    
    // 6. Spline movement validation
    ValidateSplineMovement(player, moveInfo);
    
    // 7. Time synchronization
    ValidateTimestamps(player, moveInfo);
}

void MovementChecks::ValidatePhysics(Player* player, const MovementInfo& moveInfo)
{
    const Position& newPos = moveInfo.GetPos();
    const Position& prevPos = player->GetPosition();
    
    // 1. Fall time validation
    if (moveInfo.HasMovementFlag(MOVEFLAG_FALLING))
    {
        ValidateFallPhysics(player, moveInfo);
    }
    
    // 2. Z-axis validation
    ValidateVerticalMovement(player, newPos);
    
    // 3. Collision detection
    ValidateCollision(player, prevPos, newPos);
}

void MovementChecks::ValidateFallPhysics(Player* player, const MovementInfo& moveInfo)
{
    uint32 fallTime = moveInfo.GetFallTime();
    float fallDistance = moveInfo.GetFallStart() - moveInfo.GetPos().z;
    
    // Calculate expected fall time using physics: t = √(2h/g)
    float expectedTime = sqrtf(2 * fallDistance / GRAVITY) * 1000;
    
    // Allow tolerance for network variations
    if (abs(static_cast<int32>(fallTime - expectedTime)) > FALL_TIME_TOLERANCE)
    {
        sAnticheatMgr->RecordViolation(player, CHEAT_TIME_DESYNC, 
            fmt::format("Fall time: {} vs expected {:.1f}", fallTime, expectedTime));
    }
}

void MovementChecks::ValidateVerticalMovement(Player* player, const Position& newPos)
{
    float terrainHeight = player->GetMap()->GetHeight(newPos.x, newPos.y, MAX_HEIGHT);
    float liquidLevel = player->GetMap()->GetWaterLevel(newPos.x, newPos.y);
    float zDelta = newPos.z - terrainHeight;
    
    // 1. Fly hack detection
    if (zDelta > MAX_Z_DELTA && !player->IsFlying() && !player->IsFalling())
    {
        sAnticheatMgr->RecordViolation(player, CHEAT_FLY_HACK,
            fmt::format("Z-delta: {:.2f}", zDelta));
    }
    
    // 2. Liquid state validation
    if (player->IsInWater() && newPos.z > liquidLevel + WATER_WALK_HEIGHT)
    {
        if (!player->HasAuraType(SPELL_AURA_WATER_WALK))
        {
            sAnticheatMgr->RecordViolation(player, CHEAT_WATER_WALK,
                fmt::format("Water level: {:.2f} Position: {:.2f}", liquidLevel, newPos.z));
        }
    }
}

void MovementChecks::ValidateCollision(Player* player, const Position& from, const Position& to)
{
    // Skip collision checks in instances
    if (player->GetMap()->Instanceable())
        return;
        
    Vector3 start(from.x, from.y, from.z);
    Vector3 end(to.x, to.y, to.z);
    Vector3 hitPos;
    float hitDist;
    
    // Check collision using G3D raycast
    if (player->GetMap()->GetHitPosition(start, end, hitPos, hitDist, COLLISION_TOLERANCE))
    {
        float bypassDistance = start.distance(end);
        float actualDistance = start.distance(hitPos);
        
        // Allow small tolerance for uneven terrain
        if (bypassDistance - actualDistance > COLLISION_TOLERANCE * 2)
        {
            sAnticheatMgr->RecordViolation(player, CHEAT_WALL_CLIMB,
                fmt::format("Bypassed {:.2f} units of collision", bypassDistance - actualDistance));
        }
    }
}

void MovementChecks::ValidateStateTransitions(Player* player, const MovementInfo& moveInfo)
{
    const Position& newPos = moveInfo.GetPos();
    const Position& prevPos = player->GetPosition();
    float distance = prevPos.GetExactDist(newPos);
    
    // 1. Swimming state validation
    bool wasSwimming = player->IsInWater();
    bool isSwimming = player->GetMap()->IsInWater(newPos.x, newPos.y, newPos.z);
    
    if (isSwimming != wasSwimming && distance > 5.0f)
    {
        if (!wasSwimming && !player->CanSwim())
        {
            sAnticheatMgr->RecordViolation(player, CHEAT_WATER_WALK,
                "Entered water without swimming ability");
        }
    }
    
    // 2. Flying state validation
    if (moveInfo.HasMovementFlag(MOVEFLAG_FLYING) && !player->CanFly())
    {
        sAnticheatMgr->RecordViolation(player, CHEAT_FLY_HACK,
            "Flying without ability");
    }
}

void MovementChecks::CheckTeleport(Player* player, const MovementInfo& moveInfo)
{
    const Position& prevPos = player->GetPosition();
    const Position& newPos = moveInfo.GetPos();
    float distance = prevPos.GetExactDist(newPos);
    
    // Calculate maximum possible distance
    uint32 timeDiff = moveInfo.GetTime() - player->GetLastMoveTime();
    float maxPossible = player->GetSpeed(MOVE_RUN) * (timeDiff / 1000.0f) * TELEPORT_MULTIPLIER;
    
    // Account for teleport spells and special abilities
    bool isTeleporting = player->IsTeleporting() || 
                         player->GetMotionMaster()->GetCurrentMovementGeneratorType() == TELEPORT_MOTION_TYPE;
    
    if (distance > maxPossible && !isTeleporting)
    {
        sAnticheatMgr->RecordViolation(player, CHEAT_TELEPORT, 
            fmt::format("Distance: {:.2f} > Max: {:.2f} in {}ms", distance, maxPossible, timeDiff));
    }
}

void MovementChecks::CheckSpeed(Player* player, const MovementInfo& moveInfo)
{
    const Position& prevPos = player->GetPosition();
    const Position& newPos = moveInfo.GetPos();
    float distance = prevPos.GetExactDist(newPos);
    uint32 timeDiff = moveInfo.GetTime() - player->GetLastMoveTime();
    
    if (timeDiff == 0) return; // Prevent division by zero
    
    // Calculate actual speed
    float actualSpeed = distance / (timeDiff / 1000.0f);
    
    // Get expected speed based on movement mode
    float expectedSpeed = player->GetSpeed(SelectSpeedType(moveInfo));
    
    // Apply tolerance based on movement conditions
    float tolerance = GetSpeedTolerance(player, moveInfo);
    
    if (actualSpeed > expectedSpeed * tolerance)
    {
        sAnticheatMgr->RecordViolation(player, CHEAT_SPEED_HACK, 
            fmt::format("Speed: {:.1f} > {:.1f} (Tolerance: {:.2f})", 
            actualSpeed, expectedSpeed, tolerance));
    }
}

float MovementChecks::GetSpeedTolerance(Player* player, const MovementInfo& moveInfo)
{
    float tolerance = SPEED_TOLERANCE;
    
    // Increase tolerance in complex environments
    if (player->IsInWater()) tolerance *= 1.3f;
    if (player->IsFlying()) tolerance *= 1.5f;
    if (player->IsMounted()) tolerance *= 1.4f;
    
    // Reduce tolerance on roads and paths
    if (player->GetMap()->IsOnRoad(player->GetPositionX(), player->GetPositionY()))
        tolerance *= 0.9f;
        
    return tolerance;
}

UnitMoveType MovementChecks::SelectSpeedType(const MovementInfo& moveInfo)
{
    if (moveInfo.HasMovementFlag(MOVEFLAG_SWIMMING))
        return MOVE_SWIM;
    if (moveInfo.HasMovementFlag(MOVEFLAG_FLYING))
        return MOVE_FLIGHT;
    if (moveInfo.HasMovementFlag(MOVEFLAG_WALK_MODE))
        return MOVE_WALK;
        
    return MOVE_RUN;
}

void MovementChecks::ValidateEnvironment(Player* player, const Position& newPos)
{
    // 1. Water walking validation
    if (player->m_movementInfo.HasMovementFlag(MOVEFLAG_WATERWALKING) && 
        !player->CanWaterWalk() && 
        !player->HasAuraType(SPELL_AURA_WATER_WALK))
    {
        sAnticheatMgr->RecordViolation(player, CHEAT_WATER_WALK, "Invalid water walking");
    }

    // 2. Hover validation
    if (player->m_movementInfo.HasMovementFlag(MOVEFLAG_HOVER) && 
        !player->CanHover() && 
        !player->HasAuraType(SPELL_AURA_HOVER))
    {
        sAnticheatMgr->RecordViolation(player, CHEAT_HOVER, "Invalid hover");
    }
    
    // 3. Ground validation
    if (player->m_movementInfo.HasMovementFlag(MOVEFLAG_ONTRANSPORT) && !player->GetTransport())
    {
        sAnticheatMgr->RecordViolation(player, CHEAT_TRANSPORT, "Invalid transport flag");
    }
}

void MovementChecks::ValidateSplineMovement(Player* player, const MovementInfo& moveInfo)
{
    if (!player->m_movementInfo.HasMovementFlag(MOVEFLAG_SPLINE_ENABLED))
        return;
        
    float splineHeight = player->m_movementInfo.GetSplineElevation();
    float actualHeight = moveInfo.GetPos().z - player->GetPosition().z;
    float heightDiff = fabs(splineHeight - actualHeight);
    
    if (heightDiff > 0.5f)
    {
        sAnticheatMgr->RecordViolation(player, CHEAT_SPLINE_HEIGHT, 
            fmt::format("Spline height: {:.2f} vs actual {:.2f}", splineHeight, actualHeight));
    }
    
    // Validate spline movement speed
    if (player->m_movementInfo.HasMovementFlag(MOVEFLAG_SPLINE_ENABLED))
    {
        float splineSpeed = player->m_movementInfo.GetSplineSpeed();
        float allowedSpeed = player->GetSpeed(MOVE_RUN) * 1.8f;
        
        if (splineSpeed > allowedSpeed)
        {
            sAnticheatMgr->RecordViolation(player, CHEAT_SPLINE_SPEED, 
                fmt::format("Spline speed: {:.1f} > max {:.1f}", splineSpeed, allowedSpeed));
        }
    }
}

void MovementChecks::ValidateTimestamps(Player* player, const MovementInfo& moveInfo)
{
    uint32 serverTime = WorldTimer::getMSTime();
    uint32 packetTime = moveInfo.GetTime();
    uint32 latency = player->GetSession()->GetLatency();
    
    // Calculate expected time window
    int32 timeDifference = static_cast<int32>(packetTime - serverTime);
    int32 allowedDifference = static_cast<int32>(latency) + FALL_TIME_TOLERANCE;
    
    if (abs(timeDifference) > allowedDifference)
    {
        sAnticheatMgr->RecordViolation(player, CHEAT_TIME_DESYNC, 
            fmt::format("Time diff: {}ms > allowed {}ms (Latency: {}ms)", 
            timeDifference, allowedDifference, latency));
    }
    
    // Validate fall start time consistency
    if (moveInfo.HasMovementFlag(MOVEFLAG_FALLING))
    {
        uint32 fallStartDiff = moveInfo.GetFallTime() - moveInfo.GetFallStartTime();
        if (fallStartDiff > 10000) // 10 seconds max fall
        {
            sAnticheatMgr->RecordViolation(player, CHEAT_TIME_DESYNC,
                fmt::format("Fall duration too long: {}ms", fallStartDiff));
        }
    }
}