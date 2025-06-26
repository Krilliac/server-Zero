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

#pragma once
#include "Player.h"
#include "MovementInfo.h"
#include "World.h"
#include "AnticheatMgr.h"
#include "TerrainMgr.h"
#include "G3D/Vector3.h"
#include <cmath>

class MovementChecks
{
public:
    static void FullMovementCheck(Player* player, const MovementInfo& moveInfo);
    
private:
    // Bot detection
    static void DetectMovementSharpness(Player* player);
    static void AnalyzeMovementRandomness(Player* player);
    static void DetectBotPatterns(Player* player, const MovementInfo& moveInfo);
	
    // Physics validation
    static void ValidatePhysics(Player* player, const MovementInfo& moveInfo);
    static void ValidateFallPhysics(Player* player, const MovementInfo& moveInfo);
    static void ValidateVerticalMovement(Player* player, const Position& newPos);
    static void ValidateCollision(Player* player, const Position& from, const Position& to);
    
    // State transitions
    static void ValidateStateTransitions(Player* player, const MovementInfo& moveInfo);
    
    // Anti-cheat detection
    static void CheckTeleport(Player* player, const MovementInfo& moveInfo);
    static void CheckSpeed(Player* player, const MovementInfo& moveInfo);
    
    // Environmental validation
    static void ValidateEnvironment(Player* player, const Position& newPos);
    
    // Spline validation
    static void ValidateSplineMovement(Player* player, const MovementInfo& moveInfo);
    
    // Time synchronization
    static void ValidateTimestamps(Player* player, const MovementInfo& moveInfo);
    
    // Helper functions
    static float GetSpeedTolerance(Player* player, const MovementInfo& moveInfo);
    static UnitMoveType SelectSpeedType(const MovementInfo& moveInfo);
    
	static constexpr float GRAVITY = 19.291105f; // WoW-specific gravity constant (m/s²)
	static constexpr float MAX_Z_DELTA = 5.0f;   // Max allowed height above terrain (yards)
	static constexpr float COLLISION_TOLERANCE = 0.5f; // Collision detection threshold (yards)
	static constexpr float TELEPORT_MULTIPLIER = 1.8f; // Leniency for teleport detection
	static constexpr float SPEED_TOLERANCE = 1.25f;    // Base speed tolerance (25%)
	static constexpr uint32 FALL_TIME_TOLERANCE = 500; // Fall time tolerance (ms)
	static constexpr float WATER_WALK_HEIGHT = 0.1f;   // Water walking tolerance (yards)

	// Bot detection constants
	static constexpr float GRID_SIZE = 10.0f;          // Bot detection grid size (yards)
	static constexpr uint32 MIN_WAYPOINTS_FOR_ANALYSIS = 50;
	static constexpr float BOT_LCP_THRESHOLD = 5.0f;   // LCP score threshold
	static constexpr float SHARPNESS_THRESHOLD = 0.2f; // 11.5° in radians
	static constexpr float RANDOMNESS_THRESHOLD = 0.01f; // Angle variance threshold
};