#pragma once
#include <cstdint>
#include <array>
#include <algorithm>
#include <ctime>

/**
 * @brief Implements 24-bit optimized time synchronization based on catid's work
 * 
 * Provides windowed sampling and microsecond precision for time synchronization
 */
class TimeSynchronizer
{
public:
    struct Sample
    {
        uint32_t Value;          // 24-bit timestamp value
        uint64_t Timestamp;      // Microsecond precision
        
        explicit Sample(uint32_t value = 0, uint64_t timestamp = 0)
            : Value(value & 0xFFFFFF), Timestamp(timestamp) {}
        
        bool TimeoutExpired(uint64_t now, uint64_t timeout) const
        {
            return (now - Timestamp) > timeout;
        }
    };

    TimeSynchronizer() = default;

    void Reset()
    {
        for (auto& sample : Samples) {
            sample = Sample();
        }
    }

    void Update(uint32_t value, uint64_t timestamp, uint64_t windowLength)
    {
        Samples[CurrentIndex] = Sample(value, timestamp);
        CurrentIndex = (CurrentIndex + 1) % Samples.size();
        
        // Purge expired samples
        PurgeExpired(timestamp, windowLength);
    }

    uint32_t GetBest() const
    {
        uint32_t best = 0xFFFFFFFF;
        for (const auto& sample : Samples) {
            if (sample.Value != 0 && sample.Value < best) {
                best = sample.Value;
            }
        }
        return (best == 0xFFFFFFFF) ? 0 : best;
    }

    static uint32_t LocalTimeToDatagramTS24(uint64_t usec)
    {
        return static_cast<uint32_t>(usec & 0xFFFFFF);
    }

private:
    void PurgeExpired(uint64_t now, uint64_t windowLength)
    {
        for (auto& sample : Samples) {
            if (sample.TimeoutExpired(now, windowLength)) {
                sample = Sample();
            }
        }
    }

    static constexpr size_t SAMPLE_COUNT = 3;
    std::array<Sample, SAMPLE_COUNT> Samples;
    size_t CurrentIndex = 0;
};
