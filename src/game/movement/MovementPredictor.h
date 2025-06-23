#pragma once
#include "Player.h"
#include "MovementInfo.h"
#include "Position.h"
#include "Map.h"

class MovementPredictor
{
public:
    /**
     * Calculates the predicted position based on movement info
     * 
     * @param player Player object
     * @param moveInfo Movement information
     * @return Predicted position
     */
    static Position CalculatePosition(Player* player, MovementInfo const& moveInfo);
    
    /**
     * Determines if position correction should be sent
     * 
     * @param player Player object
     * @param predicted Predicted position
     * @return true if correction is needed
     */
    static bool ShouldCorrect(Player* player, Position const& predicted);
    
    /**
     * Sends position correction to client
     * 
     * @param player Player object
     * @param correctPos Corrected position
     */
    static void SendPositionCorrection(Player* player, Position const& correctPos);

private:
    // Tolerance values (in yards)
    static constexpr float GROUND_TOLERANCE = 0.5f;
    static constexpr float SWIMMING_TOLERANCE = 0.8f;
    
    // Physics constants
    static constexpr float GRAVITY = 0.98f;
    static constexpr float WATER_DRAG = 0.85f;
    static constexpr float WATER_BUOYANCY = 0.15f;
    
    // Calculation methods
    static Position CalculateGroundPosition(Player* player, MovementInfo const& moveInfo);
    static Position CalculateSwimmingPosition(Player* player, MovementInfo const& moveInfo);
    
    // Terrain helpers
    static float GetAdjustedGroundZ(Player* player, float x, float y, float z);
    static float GetWaterLevel(Player* player, float x, float y);
};
