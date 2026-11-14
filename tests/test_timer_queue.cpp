#include "TimerQueue.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <map>
#include <vector>

using std::chrono::microseconds;
using std::chrono::milliseconds;

namespace {

const TimerQueue::Clock::time_point START = TimerQueue::Clock::now();

TimerQueue::Clock::time_point at(int ms) {
    return START + milliseconds(ms);
}

// The ids that come up at the given time, in the order they come up.
std::vector<std::uint64_t> due(TimerQueue& timers, int ms) {
    std::vector<std::uint64_t> ids;
    TimerQueue::Entry entry;

    while (timers.popDue(at(ms), entry))
        ids.push_back(entry.id);

    return ids;
}

} // namespace

TEST(TimerQueue, IsEmptyAtFirst) {
    TimerQueue timers;

    EXPECT_TRUE(due(timers, 1000000).empty());
    EXPECT_EQ(timers.msUntilNext(at(0)), -1);
}

TEST(TimerQueue, NothingComesUpBeforeItsTime) {
    TimerQueue timers;
    timers.arm(7, 1, at(100));

    EXPECT_TRUE(due(timers, 99).empty());
    EXPECT_EQ(due(timers, 100), std::vector<std::uint64_t>{1});
}

TEST(TimerQueue, ReturnsTheDescriptorAndTheTime) {
    TimerQueue timers;
    timers.arm(42, 9, at(5));

    TimerQueue::Entry entry;
    ASSERT_TRUE(timers.popDue(at(5), entry));

    EXPECT_EQ(entry.fd, 42);
    EXPECT_EQ(entry.id, 9u);
    EXPECT_EQ(entry.when, at(5));
}

TEST(TimerQueue, ComesUpEarliestFirstWhateverTheOrderOfArming) {
    TimerQueue timers;
    timers.arm(0, 3, at(300));
    timers.arm(0, 1, at(100));
    timers.arm(0, 4, at(400));
    timers.arm(0, 2, at(200));

    EXPECT_EQ(due(timers, 250), (std::vector<std::uint64_t>{1, 2}));
    EXPECT_EQ(due(timers, 1000), (std::vector<std::uint64_t>{3, 4}));
}

TEST(TimerQueue, AnIdComesUpOnlyOnce) {
    TimerQueue timers;
    timers.arm(0, 1, at(10));

    EXPECT_EQ(due(timers, 10).size(), 1u);
    EXPECT_TRUE(due(timers, 1000).empty());
}

TEST(TimerQueue, CanBeArmedAgainAfterComingUp) {
    TimerQueue timers;
    timers.arm(0, 1, at(10));
    ASSERT_EQ(due(timers, 10).size(), 1u);

    timers.arm(0, 1, at(50));

    EXPECT_TRUE(due(timers, 49).empty());
    EXPECT_EQ(due(timers, 50), std::vector<std::uint64_t>{1});
}

// The promise is "no later than". Asking for a later time leaves the
// earlier one in place and pushes nothing.
TEST(TimerQueue, ArmingLaterKeepsTheEarlierTime) {
    TimerQueue timers;
    timers.arm(0, 1, at(100));

    timers.arm(0, 1, at(500));

    EXPECT_EQ(timers.heapSize(), 1u);
    EXPECT_EQ(due(timers, 100), std::vector<std::uint64_t>{1});
    EXPECT_TRUE(due(timers, 1000).empty());
}

TEST(TimerQueue, ArmingEarlierMovesItForward) {
    TimerQueue timers;
    timers.arm(0, 1, at(500));

    timers.arm(0, 1, at(100));

    EXPECT_EQ(due(timers, 100), std::vector<std::uint64_t>{1});

    // The entry for 500 is still in the heap, out of date. It must not
    // come up.
    EXPECT_TRUE(due(timers, 1000).empty());
    EXPECT_EQ(timers.heapSize(), 0u);
}

TEST(TimerQueue, ACancelledIdNeverComesUp) {
    TimerQueue timers;
    timers.arm(0, 1, at(100));
    timers.arm(0, 2, at(200));

    timers.cancel(1);

    EXPECT_EQ(due(timers, 1000), std::vector<std::uint64_t>{2});
}

TEST(TimerQueue, CancellingAnUnknownIdDoesNothing) {
    TimerQueue timers;
    timers.arm(0, 1, at(100));

    timers.cancel(99);

    EXPECT_EQ(due(timers, 100), std::vector<std::uint64_t>{1});
}

TEST(TimerQueue, CanBeArmedAgainAfterCancelling) {
    TimerQueue timers;
    timers.arm(0, 1, at(100));
    timers.cancel(1);

    timers.arm(0, 1, at(300));

    EXPECT_TRUE(due(timers, 200).empty());
    EXPECT_EQ(due(timers, 300), std::vector<std::uint64_t>{1});
}

TEST(TimerQueue, TellsHowLongUntilTheNextOne) {
    TimerQueue timers;
    timers.arm(0, 1, at(100));
    timers.arm(0, 2, at(40));

    // Rounded up, so that sleeping this long ends after the deadline.
    EXPECT_EQ(timers.msUntilNext(at(0)), 41);
    EXPECT_EQ(timers.msUntilNext(START + microseconds(39500)), 1);
    EXPECT_EQ(timers.msUntilNext(at(40)), 0);
    EXPECT_EQ(timers.msUntilNext(at(90)), 0);
}

TEST(TimerQueue, TheNextOneIgnoresEntriesThatAreOutOfDate) {
    TimerQueue timers;
    timers.arm(0, 1, at(10));
    timers.arm(0, 2, at(500));

    timers.cancel(1);

    EXPECT_EQ(timers.msUntilNext(at(0)), 501);

    timers.cancel(2);

    EXPECT_EQ(timers.msUntilNext(at(0)), -1);
}

// A busy connection's deadline keeps moving away. That must not grow
// the heap.
TEST(TimerQueue, ADeadlineThatKeepsMovingAwayPushesNothing) {
    TimerQueue timers;
    timers.arm(0, 1, at(1000));

    for (int ms = 1001; ms < 100000; ++ms)
        timers.arm(0, 1, at(ms));

    EXPECT_EQ(timers.heapSize(), 1u);
}

// Thousands of arbitrary operations, checked step by step against a
// plain table of id to time.
TEST(TimerQueue, AgreesWithASimpleModel) {
    TimerQueue timers;
    std::map<std::uint64_t, int> model;
    unsigned state = 2026;

    auto next = [&state](unsigned limit) {
        state = state * 1103515245u + 12345u;
        return (state >> 16) % limit;
    };

    for (int now = 0; now < 4000; ++now) {
        for (int i = 0; i < 3; ++i) {
            std::uint64_t id = next(50);
            int when = now + static_cast<int>(next(200));
            unsigned action = next(10);

            if (action == 0) {
                timers.cancel(id);
                model.erase(id);
            }
            else {
                timers.arm(0, id, at(when));

                auto it = model.find(id);

                if (it == model.end() || when < it->second)
                    model[id] = when;
            }
        }

        std::vector<std::uint64_t> expected;

        for (auto it = model.begin(); it != model.end();) {
            if (it->second <= now) {
                expected.push_back(it->first);
                it = model.erase(it);
            }
            else {
                ++it;
            }
        }

        std::vector<std::uint64_t> actual = due(timers, now);
        std::sort(actual.begin(), actual.end());

        ASSERT_EQ(actual, expected) << "at time " << now;
    }
}
