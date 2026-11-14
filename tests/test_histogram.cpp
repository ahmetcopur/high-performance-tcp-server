#include "Histogram.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace {

// The percentile by its definition, on the raw samples.
std::uint64_t exactPercentile(std::vector<std::uint64_t> samples,
                              double fraction) {
    std::sort(samples.begin(), samples.end());

    auto rank = static_cast<std::size_t>(
        std::ceil(fraction * static_cast<double>(samples.size())));

    return samples[std::max<std::size_t>(rank, 1) - 1];
}

} // namespace

TEST(Histogram, IsEmptyAtFirst) {
    Histogram histogram;

    EXPECT_EQ(histogram.count(), 0u);
    EXPECT_EQ(histogram.min(), 0u);
    EXPECT_EQ(histogram.max(), 0u);
    EXPECT_EQ(histogram.mean(), 0.0);
    EXPECT_EQ(histogram.percentile(0.5), 0u);
    EXPECT_FALSE(histogram.supports(0.5));
}

TEST(Histogram, KeepsCountMinMaxAndMeanExactly) {
    Histogram histogram;

    for (std::uint64_t value : {5000u, 100u, 123456789u, 42u})
        histogram.record(value);

    EXPECT_EQ(histogram.count(), 4u);
    EXPECT_EQ(histogram.min(), 42u);
    EXPECT_EQ(histogram.max(), 123456789u);
    EXPECT_DOUBLE_EQ(histogram.mean(), (5000.0 + 100 + 123456789 + 42) / 4);
}

TEST(Histogram, SmallValuesAreExact) {
    Histogram histogram;

    for (std::uint64_t value = 0; value < 256; ++value)
        histogram.record(value);

    for (std::uint64_t value = 0; value < 256; ++value)
        EXPECT_EQ(histogram.percentile((static_cast<double>(value) + 1) / 256),
                  value);
}

// From a microsecond to ten minutes, a single value comes back within
// 1/128 of itself and never below itself.
TEST(Histogram, AnyValueIsKeptToWithinOnePartIn128) {
    for (std::uint64_t value = 1000; value < 600000000000ull;
         value = value * 17 / 16 + 3) {
        Histogram histogram;
        histogram.record(value);

        std::uint64_t bucketTop = Histogram::upperBound(0);
        histogram.forEachBucket([&bucketTop](std::uint64_t top, std::uint64_t) {
            bucketTop = top;
        });

        EXPECT_GE(bucketTop, value);
        EXPECT_LE(static_cast<double>(bucketTop - value),
                  static_cast<double>(value) / 128.0)
            << "value " << value;
    }
}

TEST(Histogram, BucketsDoNotOverlapAndLeaveNoGaps) {
    std::uint64_t previousTop = Histogram::upperBound(0);

    for (std::size_t bucket = 1; bucket < 4352; ++bucket) {
        std::uint64_t top = Histogram::upperBound(bucket);

        ASSERT_GT(top, previousTop) << "bucket " << bucket;

        // The value just above the previous bucket must land in this one.
        Histogram histogram;
        histogram.record(previousTop + 1);

        std::uint64_t landedIn = 0;
        histogram.forEachBucket([&landedIn](std::uint64_t t, std::uint64_t) {
            landedIn = t;
        });

        ASSERT_EQ(landedIn, top) << "bucket " << bucket;

        previousTop = top;
    }
}

TEST(Histogram, PercentilesMatchTheDefinitionWithinTheBucketWidth) {
    Histogram histogram;
    std::vector<std::uint64_t> samples;
    std::uint64_t state = 88172645463325252ull;

    // Latencies spread over five orders of magnitude, like a real run
    // with a long tail.
    for (int i = 0; i < 200000; ++i) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;

        std::uint64_t value = 20000 + state % 200000;

        if (i % 100 == 0)
            value *= 50;

        if (i % 10000 == 0)
            value *= 400;

        samples.push_back(value);
        histogram.record(value);
    }

    for (double fraction : {0.5, 0.9, 0.95, 0.99, 0.999, 0.9999, 1.0}) {
        std::uint64_t exact = exactPercentile(samples, fraction);
        std::uint64_t reported = histogram.percentile(fraction);

        EXPECT_GE(reported, exact) << "fraction " << fraction;
        EXPECT_LE(static_cast<double>(reported - exact),
                  static_cast<double>(exact) / 128.0)
            << "fraction " << fraction;
    }
}

TEST(Histogram, TheHundredthPercentileIsTheMaximum) {
    Histogram histogram;

    for (std::uint64_t value : {1000000u, 2000000u, 987654321u})
        histogram.record(value);

    EXPECT_EQ(histogram.percentile(1.0), 987654321u);
}

TEST(Histogram, MergingIsTheSameAsRecordingEverythingInOne) {
    Histogram first;
    Histogram second;
    Histogram both;

    for (std::uint64_t i = 1; i <= 5000; ++i) {
        std::uint64_t value = i * i * 37 + 1000;

        (i % 3 == 0 ? first : second).record(value);
        both.record(value);
    }

    first.merge(second);

    EXPECT_EQ(first.count(), both.count());
    EXPECT_EQ(first.min(), both.min());
    EXPECT_EQ(first.max(), both.max());
    EXPECT_DOUBLE_EQ(first.mean(), both.mean());

    for (double fraction : {0.01, 0.5, 0.99, 0.999})
        EXPECT_EQ(first.percentile(fraction), both.percentile(fraction));
}

TEST(Histogram, MergingAnEmptyOneChangesNothing) {
    Histogram histogram;
    histogram.record(5000);

    histogram.merge(Histogram());

    EXPECT_EQ(histogram.count(), 1u);
    EXPECT_EQ(histogram.min(), 5000u);

    Histogram empty;
    empty.merge(histogram);

    EXPECT_EQ(empty.count(), 1u);
    EXPECT_EQ(empty.min(), 5000u);
    EXPECT_EQ(empty.max(), 5000u);
}

// A percentile needs ten samples above it.
TEST(Histogram, SaysWhetherAPercentileHasEnoughSamplesBehindIt) {
    Histogram histogram;

    for (int i = 0; i < 999; ++i)
        histogram.record(1000);

    EXPECT_TRUE(histogram.supports(0.5));
    EXPECT_TRUE(histogram.supports(0.95));
    EXPECT_FALSE(histogram.supports(0.99));

    histogram.record(1000);

    EXPECT_TRUE(histogram.supports(0.99));
    EXPECT_FALSE(histogram.supports(0.999));

    for (int i = 0; i < 9000; ++i)
        histogram.record(1000);

    EXPECT_TRUE(histogram.supports(0.999));
}

TEST(Histogram, ValuesBeyondTheRangeShareTheLastBucketButKeepTheirMaximum) {
    Histogram histogram;
    std::uint64_t huge = 1ull << 50;

    histogram.record(huge);
    histogram.record(huge * 2);

    EXPECT_EQ(histogram.count(), 2u);
    EXPECT_EQ(histogram.max(), huge * 2);
    EXPECT_EQ(histogram.percentile(1.0), huge * 2);
}

TEST(Histogram, VisitsOnlyBucketsThatHaveSamples) {
    Histogram histogram;
    histogram.record(10);
    histogram.record(10);
    histogram.record(1000000);

    std::vector<std::uint64_t> bucketCounts;
    std::uint64_t total = 0;

    histogram.forEachBucket([&](std::uint64_t, std::uint64_t count) {
        bucketCounts.push_back(count);
        total += count;
    });

    EXPECT_EQ(bucketCounts, (std::vector<std::uint64_t>{2, 1}));
    EXPECT_EQ(total, histogram.count());
}
