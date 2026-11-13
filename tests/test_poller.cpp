#include "FileDescriptor.h"
#include "PollPoller.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <vector>

#include <sys/socket.h>
#include <unistd.h>

namespace {

struct Pipe {
    FileDescriptor readEnd;
    FileDescriptor writeEnd;

    Pipe() {
        int fds[2];
        EXPECT_EQ(pipe(fds), 0);
        readEnd.reset(fds[0]);
        writeEnd.reset(fds[1]);
    }

    void fill() {
        EXPECT_EQ(write(writeEnd.get(), "x", 1), 1);
    }
};

struct SocketPair {
    FileDescriptor first;
    FileDescriptor second;

    SocketPair() {
        int fds[2];
        EXPECT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
        first.reset(fds[0]);
        second.reset(fds[1]);
    }
};

std::vector<int> readableFds(PollPoller& poller) {
    std::vector<PollEvent> events;
    EXPECT_TRUE(poller.wait(0, events));

    std::vector<int> fds;

    for (const PollEvent& event : events) {
        if (event.readable)
            fds.push_back(event.fd);
    }

    std::sort(fds.begin(), fds.end());
    return fds;
}

} // namespace

TEST(PollPoller, ReportsNothingWhenNothingIsReady) {
    Pipe pipe;
    PollPoller poller;
    poller.add(pipe.readEnd.get(), true, false);

    std::vector<PollEvent> events;

    EXPECT_TRUE(poller.wait(0, events));
    EXPECT_TRUE(events.empty());
}

TEST(PollPoller, ReportsReadableWhenDataArrives) {
    Pipe pipe;
    PollPoller poller;
    poller.add(pipe.readEnd.get(), true, false);

    pipe.fill();
    std::vector<PollEvent> events;

    ASSERT_TRUE(poller.wait(0, events));
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].fd, pipe.readEnd.get());
    EXPECT_TRUE(events[0].readable);
    EXPECT_FALSE(events[0].writable);
    EXPECT_FALSE(events[0].error);
}

TEST(PollPoller, ReportsWritableWhenThereIsRoom) {
    SocketPair pair;
    PollPoller poller;
    poller.add(pair.first.get(), false, true);

    std::vector<PollEvent> events;

    ASSERT_TRUE(poller.wait(0, events));
    ASSERT_EQ(events.size(), 1u);
    EXPECT_TRUE(events[0].writable);
    EXPECT_FALSE(events[0].readable);
}

TEST(PollPoller, IgnoresReadinessNobodyAskedFor) {
    Pipe pipe;
    PollPoller poller;
    poller.add(pipe.readEnd.get(), false, false);
    pipe.fill();

    EXPECT_TRUE(readableFds(poller).empty());

    poller.modify(pipe.readEnd.get(), true, false);

    EXPECT_EQ(readableFds(poller), std::vector<int>{pipe.readEnd.get()});

    poller.modify(pipe.readEnd.get(), false, false);

    EXPECT_TRUE(readableFds(poller).empty());
}

TEST(PollPoller, ReplacesTheEventsOfThePreviousWait) {
    Pipe pipe;
    PollPoller poller;
    poller.add(pipe.readEnd.get(), true, false);
    pipe.fill();

    std::vector<PollEvent> events;
    ASSERT_TRUE(poller.wait(0, events));
    ASSERT_EQ(events.size(), 1u);

    poller.modify(pipe.readEnd.get(), false, false);
    ASSERT_TRUE(poller.wait(0, events));

    EXPECT_TRUE(events.empty());
}

// Removing takes the last entry into the freed place. Every remaining
// descriptor must still be found afterwards, wherever it was.
TEST(PollPoller, KeepsWatchingTheOthersAfterARemoval) {
    std::vector<Pipe> pipes(5);
    PollPoller poller;
    std::vector<int> expected;

    for (Pipe& pipe : pipes) {
        poller.add(pipe.readEnd.get(), true, false);
        pipe.fill();
        expected.push_back(pipe.readEnd.get());
    }

    std::sort(expected.begin(), expected.end());
    ASSERT_EQ(readableFds(poller), expected);

    // First, middle, last, then the rest.
    for (std::size_t index : {0u, 2u, 4u, 1u, 3u}) {
        int fd = pipes[index].readEnd.get();

        poller.remove(fd);
        expected.erase(std::find(expected.begin(), expected.end(), fd));

        EXPECT_EQ(readableFds(poller), expected);
    }
}

TEST(PollPoller, ADescriptorCanBeAddedAgainAfterRemoval) {
    Pipe pipe;
    PollPoller poller;
    pipe.fill();

    poller.add(pipe.readEnd.get(), true, false);
    poller.remove(pipe.readEnd.get());
    poller.add(pipe.readEnd.get(), true, false);

    EXPECT_EQ(readableFds(poller), std::vector<int>{pipe.readEnd.get()});
}

// Linux reports the closed peer as readable (the read returns 0), macOS
// as readable and hangup. Either way the owner is told to look.
TEST(PollPoller, ReportsAClosedPeer) {
    SocketPair pair;
    PollPoller poller;
    poller.add(pair.first.get(), true, false);

    pair.second.reset();
    std::vector<PollEvent> events;

    ASSERT_TRUE(poller.wait(0, events));
    ASSERT_EQ(events.size(), 1u);
    EXPECT_TRUE(events[0].readable || events[0].hangup);
}

TEST(PollPoller, ReportsADescriptorThatIsNotOpenAsAnError) {
    Pipe pipe;
    PollPoller poller;
    int fd = pipe.readEnd.get();
    poller.add(fd, true, false);

    pipe.readEnd.reset();
    std::vector<PollEvent> events;

    ASSERT_TRUE(poller.wait(0, events));
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].fd, fd);
    EXPECT_TRUE(events[0].error);
}

TEST(PollPoller, ReturnsAfterTheTimeout) {
    Pipe pipe;
    PollPoller poller;
    poller.add(pipe.readEnd.get(), true, false);

    std::vector<PollEvent> events;
    auto start = std::chrono::steady_clock::now();

    EXPECT_TRUE(poller.wait(50, events));

    auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_TRUE(events.empty());
    EXPECT_GE(elapsed, std::chrono::milliseconds(40));
}
