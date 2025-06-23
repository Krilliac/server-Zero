#pragma once
#include "Player.h"
#include <cstdint>

class TimeSync
{
public:
    /**
     * Calculates the time offset between client and server.
     * 
     * @param player Player object for context
     * @param clientTime Timestamp from client (ms)
     * @param serverTime Timestamp from server (ms)
     * @param rtt Round-trip time (ms)
     * @return Calculated time offset (ms)
     */
    static int32 CalculateOffset(Player* player, uint32 clientTime, uint32 serverTime, uint32 rtt);
    
    /**
     * Converts local time to 24-bit timestamp for network optimization.
     * 
     * @param usec Microsecond-precision timestamp
     * @return 24-bit timestamp
     */
    static uint32_t LocalTimeToDatagramTS24(uint64_t usec);
    
    /**
     * Applies Kalman filter to smooth time offset updates.
     * 
     * @param newOffset Newly calculated offset
     * @param lastOffset Previous offset value
     * @param gain Kalman gain factor (0.0-1.0)
     * @return Filtered time offset
     */
    static int32 ApplyKalmanFilter(int32 newOffset, int32 lastOffset, float gain);
};
