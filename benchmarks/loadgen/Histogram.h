#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

// A latency histogram with logarithmic buckets, in the style of
// HdrHistogram. Values are whole nanoseconds.
//
// Every power of two is cut into 128 buckets of equal width, so a value
// is kept to within 1/128 of itself, under 0.8 %, whether it is a
// microsecond or a minute. Values below 256 are kept exactly. Recording
// is one increment in an array of 4,352 counters, so it costs the same
// for the first sample and the hundred-millionth, and two histograms
// are merged by adding their arrays.
//
// Averages hide the slow requests. This keeps the whole distribution, so
// that the 99th percentile and the maximum can be read off it.
class Histogram {
private:
    static constexpr int SUB_BITS = 7;
    static constexpr std::uint64_t SUB_COUNT = 1ull << SUB_BITS;

    // Values below this have a bucket each.
    static constexpr std::uint64_t EXACT_LIMIT = SUB_COUNT * 2;

    // Values from here up share the last bucket. About 18 minutes.
    static constexpr int MAX_BITS = 40;

    static constexpr std::size_t BUCKETS =
        EXACT_LIMIT + (MAX_BITS - SUB_BITS - 1) * SUB_COUNT;

    std::vector<std::uint64_t> counts;
    std::uint64_t total = 0;
    std::uint64_t sum = 0;
    std::uint64_t smallest = 0;
    std::uint64_t largest = 0;

    // Position of the highest set bit.
    static int topBit(std::uint64_t value) {
        int bit = 0;

        while (value >>= 1)
            ++bit;

        return bit;
    }

    static std::size_t bucketOf(std::uint64_t value) {
        if (value < EXACT_LIMIT)
            return static_cast<std::size_t>(value);

        int top = topBit(value);

        if (top >= MAX_BITS)
            return BUCKETS - 1;

        // The top bit and the seven below it: 128 to 255.
        std::uint64_t leading = value >> (top - SUB_BITS);

        return static_cast<std::size_t>(
            EXACT_LIMIT +
            static_cast<std::uint64_t>(top - SUB_BITS - 1) * SUB_COUNT +
            (leading - SUB_COUNT));
    }

public:
    Histogram() : counts(BUCKETS, 0) {
    }

    // The largest value that lands in a bucket.
    static std::uint64_t upperBound(std::size_t bucket) {
        if (bucket < EXACT_LIMIT)
            return bucket;

        std::uint64_t offset = bucket - EXACT_LIMIT;
        int shift = static_cast<int>(offset / SUB_COUNT) + 1;
        std::uint64_t leading = SUB_COUNT + offset % SUB_COUNT;

        return ((leading + 1) << shift) - 1;
    }

    void record(std::uint64_t value) {
        ++counts[bucketOf(value)];

        if (total == 0 || value < smallest)
            smallest = value;

        if (value > largest)
            largest = value;

        ++total;
        sum += value;
    }

    void merge(const Histogram& other) {
        if (other.total == 0)
            return;

        for (std::size_t i = 0; i < BUCKETS; ++i)
            counts[i] += other.counts[i];

        if (total == 0 || other.smallest < smallest)
            smallest = other.smallest;

        largest = std::max(largest, other.largest);
        total += other.total;
        sum += other.sum;
    }

    std::uint64_t count() const {
        return total;
    }

    // Exact, not rounded to a bucket.
    std::uint64_t min() const {
        return smallest;
    }

    std::uint64_t max() const {
        return largest;
    }

    double mean() const {
        return total == 0
                   ? 0.0
                   : static_cast<double>(sum) / static_cast<double>(total);
    }

    // The value that the given fraction of the samples do not exceed,
    // for example 0.99 for the 99th percentile. It is the top of the
    // bucket that sample falls in, so it can overstate by up to 0.8 %
    // and never understates. 0 if nothing was recorded.
    std::uint64_t percentile(double fraction) const {
        if (total == 0)
            return 0;

        auto wanted =
            static_cast<std::uint64_t>(fraction * static_cast<double>(total));

        if (static_cast<double>(wanted) < fraction * static_cast<double>(total))
            ++wanted;

        wanted = std::clamp<std::uint64_t>(wanted, 1, total);

        std::uint64_t seen = 0;

        for (std::size_t i = 0; i < BUCKETS; ++i) {
            seen += counts[i];

            // The last bucket has no top of its own: it holds everything
            // beyond the range.
            if (seen >= wanted)
                return i == BUCKETS - 1 ? largest
                                        : std::min(upperBound(i), largest);
        }

        return largest;
    }

    // Whether there are enough samples for that percentile to mean
    // something. It needs at least ten samples above it: with 500
    // samples the 99.9th percentile is decided by a single request.
    bool supports(double fraction) const {
        return static_cast<double>(total) * (1.0 - fraction) >= 10.0;
    }

    // Calls visit(upper bound, count) for every bucket that is not empty,
    // smallest values first.
    template<typename Visit> void forEachBucket(Visit&& visit) const {
        for (std::size_t i = 0; i < BUCKETS; ++i) {
            if (counts[i] != 0)
                visit(upperBound(i), counts[i]);
        }
    }
};
