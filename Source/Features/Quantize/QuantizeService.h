#pragma once

#include <cstdint>
#include <cmath>
#include <algorithm>

/// Pure, stateless utility for snapping sample positions to beat positions.
/// All methods are O(1), allocation-free, lock-free — safe for any thread including audio.
class QuantizeService
{
public:
    QuantizeService() = delete;

    /// Returns the beat position closest to `position` on the grid defined by anchor + interval.
    static int64_t snapToNearestBeat (int64_t position, int64_t anchor, double beatInterval)
    {
        if (beatInterval <= 0.0)
            return position;

        double offset = static_cast<double> (position - anchor);
        double beatIndex = std::round (offset / beatInterval);
        int64_t result = anchor + static_cast<int64_t> (beatIndex * beatInterval);
        return std::max (result, int64_t (0));
    }

    /// Returns the first beat position strictly after `position`.
    static int64_t getNextBeatAfter (int64_t position, int64_t anchor, double beatInterval)
    {
        if (beatInterval <= 0.0)
            return position;

        double offset = static_cast<double> (position - anchor);
        double beatIndex = std::floor (offset / beatInterval) + 1.0;
        int64_t result = anchor + static_cast<int64_t> (beatIndex * beatInterval);
        return std::max (result, int64_t (0));
    }

    /// Returns the last beat position strictly before `position`.
    static int64_t getPreviousBeatBefore (int64_t position, int64_t anchor, double beatInterval)
    {
        if (beatInterval <= 0.0)
            return position;

        double offset = static_cast<double> (position - anchor);
        double beatIndex = std::ceil (offset / beatInterval) - 1.0;
        int64_t result = anchor + static_cast<int64_t> (beatIndex * beatInterval);
        return std::max (result, int64_t (0));
    }
};
