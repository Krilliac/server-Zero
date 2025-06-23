#include "PhysicsValidator.h"
#include "AntiCheatMgr.h"
#include "Map.h"
#include "Log.h"
#include "World.h"
#include <cmath>

bool PhysicsValidator::ValidatePosition(Player* player, MovementInfo const& moveInfo)
{
    const Position& newPos = moveInfo.GetPos();
    
    // 1. Check for valid map coordinates
    if (!Map::IsValidMapCoord(newPos.x, newPos.y, newPos.z, newPos.o)) {
        sAntiCheatMgr->RecordViolation(player, CHEAT_PHYSICS_HACK, "Invalid map coordinates");
        return false;
    }
    
    // 2. Check if position is walkable
    if (!IsPositionValid(player, newPos)) {
        sAntiCheatMgr->RecordViolation(player, CHEAT_PHYSICS_HACK, "Unwalkable position");
        return false;
    }
    
    // 3. Validate falling physics
    if (!ValidateFalling(player, moveInfo)) {
        return false;
    }
    
    // 4. Validate swimming physics
    if (player->IsSwimming() && !ValidateSwimming(player, moveInfo)) {
        return false;
    }
    
    // 5. Validate flying physics
    if (player->IsFlying() && !ValidateFlying(player, moveInfo)) {
        return false;
    }
    
    return true;
}

bool PhysicsValidator::ValidateFalling(Player* player, MovementInfo const& moveInfo)
{
    if (!moveInfo.HasMovementFlag(MOVEFLAG_FALLING)) {
        return true;
    }
    
    const float fallDistance = CalculateFallDistance(moveInfo);
    
    if (fallDistance > MAX_FALL_DISTANCE) {
        sAntiCheatMgr->RecordViolation(player, CHEAT_WALL_CLIMB, 
            fmt::format("Fall distance: {:.1f}m", fallDistance));
        return false;
    }
    
    // Check for valid landing position
    const Position& landPos = moveInfo.GetPos();
    float groundZ = player->GetMap()->GetHeight(landPos.x, landPos.y, landPos.z);
    
    if (groundZ > INVALID_HEIGHT && std::abs(landPos.z - groundZ) > 5.0f) {
        sAntiCheatMgr->RecordViolation(player, CHEAT_PHYSICS_HACK,
            fmt::format("Invalid landing: Z={:.2f}, Ground={:.2f}", landPos.z, groundZ));
        return false;
    }
    
    return true;
}

bool PhysicsValidator::ValidateSwimming(Player* player, MovementInfo const& moveInfo)
{
    const Position& newPos = moveInfo.GetPos();
    float waterLevel = player->GetMap()->GetWaterLevel(newPos.x, newPos.y);
    
    // 1. Check if player is in water area
    if (waterLevel == INVALID_HEIGHT) {
        sAntiCheatMgr->RecordViolation(player, CHEAT_SWIMMING_HACK, "Swimming without water");
        return false;
    }
    
    // 2. Check surface tolerance
    if (std::abs(newPos.z - waterLevel) > WATER_SURFACE_TOLERANCE) {
        sAntiCheatMgr->RecordViolation(player, CHEAT_SWIMMING_HACK,
            fmt::format("Z-pos: {:.2f}, Water: {:.2f}", newPos.z, waterLevel));
        return false;
    }
    
    // 3. Validate vertical speed
    const float verticalSpeed = CalculateVerticalSpeed(moveInfo);
    const float maxSpeed = player->GetSpeed(MOVE_SWIM) * 1.5f;
    
    if (std::abs(verticalSpeed) > maxSpeed) {
        sAntiCheatMgr->RecordViolation(player, CHEAT_SWIMMING_HACK,
            fmt::format("Vertical speed: {:.1f} > max {:.1f}", verticalSpeed, maxSpeed));
        return false;
    }
    
    return true;
}

bool PhysicsValidator::ValidateFlying(Player* player, MovementInfo const& moveInfo)
{
    if (!player->IsFlying()) {
        return true;
    }
    
    const Position& newPos = moveInfo.GetPos();
    
    // 1. Check flight height limits
    if (newPos.z > MAX_FLIGHT_HEIGHT || newPos.z < MIN_FLIGHT_HEIGHT) {
        sAntiCheatMgr->RecordViolation(player, CHEAT_FLY_HACK,
            fmt::format("Flight height: {:.1f}", newPos.z));
        return false;
    }
    
    // 2. Validate vertical speed
    const float verticalSpeed = CalculateVerticalSpeed(moveInfo);
    const float maxSpeed = player->GetSpeed(MOVE_FLIGHT) * 1.2f;
    
    if (std::abs(verticalSpeed) > maxSpeed) {
        sAntiCheatMgr->RecordViolation(player, CHEAT_FLY_HACK,
            fmt::format("Vertical speed: {:.1f} > max {:.1f}", verticalSpeed, maxSpeed));
        return false;
    }
    
    return true;
}

bool PhysicsValidator::IsPositionValid(Player* player, Position const& pos)
{
    Map* map = player->GetMap();
    
    // 1. Check ground Z
    float groundZ = map->GetHeight(pos.x, pos.y, pos.z);
    if (groundZ > INVALID_HEIGHT && pos.z < groundZ - 10.0f) {
        return false; // Fell through world
    }
    
    // 2. Check collision
    if (map->IsInWater(pos.x, pos.y, pos.z)) {
        float waterLevel = map->GetWaterLevel(pos.x, pos.y);
        if (waterLevel == INVALID_HEIGHT || pos.z < waterLevel - 20.0f) {
            return false;
        }
    }
    
    // 3. Check collision with objects
    if (map->GetGameObjectCollision(pos.x, pos.y, pos.z, 1.0f)) {
        return false;
    }
    
    return true;
}

float PhysicsValidator::CalculateFallDistance(MovementInfo const& moveInfo)
{
    if (!moveInfo.HasMovementFlag(MOVEFLAG_FALLING)) {
        return 0.0f;
    }
    
    const float gravity = 0.98f;
    const float fallTime = moveInfo.fallTime / 1000.0f; // Convert to seconds
    return 0.5f * gravity * fallTime * fallTime;
}

float PhysicsValidator::CalculateVerticalSpeed(MovementInfo const& moveInfo)
{
    if (moveInfo.jump.xyspeed == 0) {
        return 0.0f;
    }
    
    const float deltaTime = (moveInfo.time - moveInfo.prevMoveTime) / 1000.0f;
    if (deltaTime <= 0) {
        return 0.0f;
    }
    
    return (moveInfo.jump.zspeed - moveInfo.jump.startZSpeed) / deltaTime;
}
