#include "MovementPredictor.h"
#include "WorldSession.h"
#include "Log.h"
#include "MapManager.h"
#include "ObjectAccessor.h"
#include "MovementChecks.h"
#include "GameTime.h"
#include <cmath>

Position MovementPredictor::CalculatePosition(Player* player, MovementInfo const& moveInfo)
{
    if (moveInfo.HasMovementFlag(MOVEFLAG_SWIMMING))
        return CalculateSwimmingPosition(player, moveInfo);
    else
        return CalculateGroundPosition(player, moveInfo);
}

Position MovementPredictor::CalculateGroundPosition(Player* player, MovementInfo const& moveInfo)
{
    Position newPos = moveInfo.GetPos();
    float groundZ = GetAdjustedGroundZ(player, newPos.x, newPos.y, newPos.z);
    
    if (groundZ > INVALID_HEIGHT) {
        // Calculate fall distance with terrain-adaptive gravity
        float terrainGravity = GRAVITY;
        if (player->IsInWater()) 
            terrainGravity *= 0.3f;
        else if (player->IsFlying()) 
            terrainGravity = 0.0f;
        
        const float fallTime = (moveInfo.fallTime / 1000.0f);
        const float fallDistance = 0.5f * terrainGravity * fallTime * fallTime;
        
        // Apply fall distance with ground clamping
        newPos.z = std::max(groundZ, newPos.z - fallDistance);
        
        // Slope compensation
        float slopeFactor = player->GetMap()->GetSlope(newPos.x, newPos.y);
        newPos.z += 0.2f * slopeFactor;
    }
    
    return newPos;
}

Position MovementPredictor::CalculateSwimmingPosition(Player* player, MovementInfo const& moveInfo)
{
    Position newPos = moveInfo.GetPos();
    const float speed = player->GetSpeed(MOVE_SWIM);
    const float direction = moveInfo.GetOrientation();
    const float timeDelta = (moveInfo.time - player->m_movementInfo.time) / 1000.0f;
    
    // Water physics model with depth awareness
    const bool isDeepWater = player->IsInDeepWater();
    const float drag = isDeepWater ? 0.75f : WATER_DRAG;
    
    // Movement calculation
    newPos.x += (speed * drag) * timeDelta * std::cos(direction);
    newPos.y += (speed * drag) * timeDelta * std::sin(direction);
    
    // Buoyancy effect
    newPos.z += WATER_BUOYANCY * timeDelta;
    
    // Water surface clamping
    float waterLevel = GetWaterLevel(player, newPos.x, newPos.y);
    if (waterLevel > INVALID_HEIGHT) {
        // Keep player within valid water bounds
        const float surfaceThreshold = 0.5f;
        const float maxDepth = 20.0f;
        
        if (newPos.z > waterLevel - surfaceThreshold) {
            newPos.z = waterLevel - surfaceThreshold;
        } else if (newPos.z < waterLevel - maxDepth) {
            newPos.z = waterLevel - maxDepth;
        }
    }
    
    return newPos;
}

bool MovementPredictor::ShouldCorrect(Player* player, Position const& predicted)
{
    const Position& current = player->GetPosition();
    const float distance = current.GetExactDist2d(predicted);
    
    return player->IsSwimming() ? (distance > SWIMMING_TOLERANCE) : 
                                  (distance > GROUND_TOLERANCE);
}

void MovementPredictor::SendPositionCorrection(Player* player, Position const& correctPos)
{
    WorldPacket data(SMSG_MOVE_SET_POSITION, 20);
    data << player->GetPackGUID();
    data << float(correctPos.x);
    data << float(correctPos.y);
    data << float(correctPos.z);
    data << float(correctPos.o);
    player->SendMessageToSet(&data, true);
    
    // Update player state
    player->SetPosition(correctPos);
    player->SetLastSafePosition(correctPos);
}

float MovementPredictor::GetAdjustedGroundZ(Player* player, float x, float y, float z)
{
    Map* map = player->GetMap();
    float groundZ = map->GetHeight(x, y, z);
    
    // Adjust for possible flying/swimming states
    if (player->IsFlying() && map->IsFlyingEnabled()) {
        return z; // Flying ignores ground Z
    }
    
    // Water surface handling
    if (player->IsSwimming()) {
        float waterLevel = map->GetWaterLevel(x, y);
        if (waterLevel > INVALID_HEIGHT) {
            return std::max(groundZ, waterLevel - 0.5f);
        }
    }
    
    return groundZ;
}

float MovementPredictor::GetWaterLevel(Player* player, float x, float y)
{
    return player->GetMap()->GetWaterLevel(x, y);
}
