#pragma once
#include "Player.h"
#include "MovementInfo.h"
#include "Map.h"

class PhysicsValidator
{
public:
    /**
     * Validates player position against world physics
     * 
     * @param player Player object
     * @param moveInfo Movement information
     * @return true if position is valid
     */
    static bool ValidatePosition(Player* player, MovementInfo const& moveInfo);
    
    /**
     * Validates falling physics
     * 
     * @param player Player object
     * @param moveInfo Movement information
     * @return true if falling is valid
     */
    static bool ValidateFalling(Player* player, MovementInfo const& moveInfo);
    
    /**
     * Validates swimming physics
     * 
     * @param player Player object
     * @param moveInfo Movement information
     * @return true if swimming is valid
     */
    static bool ValidateSwimming(Player* player, MovementInfo const& moveInfo);
    
    /**
     * Validates flying physics
     * 
     * @param player Player object
     * @param moveInfo Movement information
     * @return true if flying is valid
     */
    static bool ValidateFlying(Player* player, MovementInfo const& moveInfo);

private:
    // Physics constants
    static constexpr float MAX_FALL_DISTANCE = 50.0f;
    static constexpr float WATER_SURFACE_TOLERANCE = 0.5f;
    static constexpr float MAX_FLIGHT_HEIGHT = 200.0f;
    static constexpr float MIN_FLIGHT_HEIGHT = -100.0f;
    
    // Helper functions
    static bool IsPositionValid(Player* player, Position const& pos);
    static float CalculateFallDistance(MovementInfo const& moveInfo);
    static float CalculateVerticalSpeed(MovementInfo const& moveInfo);
};
