#include "TimeSync.h"
#include "Config.h"
#include "Log.h"
#include <cmath>

int32 TimeSync::CalculateOffset(Player* player, uint32 clientTime, uint32 serverTime, uint32 rtt)
{
    // Offset = (serverTime - clientTime) - (rtt / 2)
    return static_cast<int32>(serverTime) - static_cast<int32>(clientTime) - static_cast<int32>(rtt / 2);
}

uint32_t TimeSync::LocalTimeToDatagramTS24(uint64_t usec)
{
    // Extract 24 LSBs for optimized network transmission
    return static_cast<uint32_t>(usec & 0xFFFFFF);
}

int32 TimeSync::ApplyKalmanFilter(int32 newOffset, int32 lastOffset, float gain)
{
    // Kalman filter: filtered = gain * new + (1 - gain) * last
    return static_cast<int32>(gain * newOffset + (1 - gain) * lastOffset);
}
