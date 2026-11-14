#include "Waker.h"

#include <gtest/gtest.h>

#include <cerrno>
#include <chrono>
#include <thread>

#include <poll.h>

namespace {

bool readable(const Waker& waker, int timeoutMs = 0) {
    pollfd entry{waker.fd(), POLLIN, 0};

    return poll(&entry, 1, timeoutMs) == 1 && (entry.revents & POLLIN);
}

} // namespace

TEST(Waker, IsQuietUntilNotified) {
    Waker waker;

    EXPECT_FALSE(readable(waker));
}

TEST(Waker, NotifyMakesTheDescriptorReadable) {
    Waker waker;

    waker.notify();

    EXPECT_TRUE(readable(waker));
}

TEST(Waker, DrainMakesItQuietAgain) {
    Waker waker;
    waker.notify();

    waker.drain();

    EXPECT_FALSE(readable(waker));

    waker.notify();

    EXPECT_TRUE(readable(waker));
}

TEST(Waker, ManyNotifiesAreClearedByOneDrain) {
    Waker waker;

    for (int i = 0; i < 1000; ++i)
        waker.notify();

    waker.drain();

    EXPECT_FALSE(readable(waker));
}

// Far more than a pipe holds. notify() must return every time, and the
// descriptor must still work afterwards.
TEST(Waker, NotifyNeverBlocks) {
    Waker waker;

    for (int i = 0; i < 300000; ++i)
        waker.notify();

    EXPECT_TRUE(readable(waker));

    waker.drain();

    EXPECT_FALSE(readable(waker));

    waker.notify();

    EXPECT_TRUE(readable(waker));
}

// It runs inside signal handlers, where changing errno would corrupt
// whatever the interrupted code was about to check.
TEST(Waker, NotifyLeavesErrnoAlone) {
    Waker waker;

    for (int i = 0; i < 300000; ++i)
        waker.notify();

    errno = EDOM;
    waker.notify();

    EXPECT_EQ(errno, EDOM);
}

TEST(Waker, WakesAThreadThatIsWaiting) {
    Waker waker;

    std::thread other([&waker]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        waker.notify();
    });

    EXPECT_TRUE(readable(waker, 5000));

    other.join();
}
