#include "EpollPoller.h"
#include "FileDescriptor.h"
#include "PollPoller.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <set>
#include <string>
#include <type_traits>
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

std::vector<int> readableFds(Poller& poller) {
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

// Every test in this suite runs against each poller the platform has,
// so that the two are held to the same behaviour.
template<typename T> class PollerTest : public ::testing::Test {
protected:
    T poller;
};

#ifdef __linux__
using PollerTypes = ::testing::Types<PollPoller, EpollPoller>;
#else
using PollerTypes = ::testing::Types<PollPoller>;
#endif

class PollerNames {
public:
    template<typename T> static std::string GetName(int) {
        return std::is_same<T, PollPoller>::value ? "poll" : "epoll";
    }
};

TYPED_TEST_SUITE(PollerTest, PollerTypes, PollerNames);

} // namespace

TYPED_TEST(PollerTest, ReportsNothingWhenNothingIsReady) {
    Pipe pipe;
    this->poller.add(pipe.readEnd.get(), true, false);

    std::vector<PollEvent> events;

    EXPECT_TRUE(this->poller.wait(0, events));
    EXPECT_TRUE(events.empty());
}

TYPED_TEST(PollerTest, ReportsReadableWhenDataArrives) {
    Pipe pipe;
    this->poller.add(pipe.readEnd.get(), true, false);

    pipe.fill();
    std::vector<PollEvent> events;

    ASSERT_TRUE(this->poller.wait(0, events));
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].fd, pipe.readEnd.get());
    EXPECT_TRUE(events[0].readable);
    EXPECT_FALSE(events[0].writable);
    EXPECT_FALSE(events[0].error);
}

TYPED_TEST(PollerTest, ReportsWritableWhenThereIsRoom) {
    SocketPair pair;
    this->poller.add(pair.first.get(), false, true);

    std::vector<PollEvent> events;

    ASSERT_TRUE(this->poller.wait(0, events));
    ASSERT_EQ(events.size(), 1u);
    EXPECT_TRUE(events[0].writable);
    EXPECT_FALSE(events[0].readable);
}

TYPED_TEST(PollerTest, IgnoresReadinessNobodyAskedFor) {
    Pipe pipe;
    this->poller.add(pipe.readEnd.get(), false, false);
    pipe.fill();

    EXPECT_TRUE(readableFds(this->poller).empty());

    this->poller.modify(pipe.readEnd.get(), true, false);

    EXPECT_EQ(readableFds(this->poller), std::vector<int>{pipe.readEnd.get()});

    this->poller.modify(pipe.readEnd.get(), false, false);

    EXPECT_TRUE(readableFds(this->poller).empty());
}

TYPED_TEST(PollerTest, ReplacesTheEventsOfThePreviousWait) {
    Pipe pipe;
    this->poller.add(pipe.readEnd.get(), true, false);
    pipe.fill();

    std::vector<PollEvent> events;
    ASSERT_TRUE(this->poller.wait(0, events));
    ASSERT_EQ(events.size(), 1u);

    this->poller.modify(pipe.readEnd.get(), false, false);
    ASSERT_TRUE(this->poller.wait(0, events));

    EXPECT_TRUE(events.empty());
}

// Removing takes the last entry into the freed place. Every remaining
// descriptor must still be found afterwards, wherever it was.
TYPED_TEST(PollerTest, KeepsWatchingTheOthersAfterARemoval) {
    std::vector<Pipe> pipes(5);
    std::vector<int> expected;

    for (Pipe& pipe : pipes) {
        this->poller.add(pipe.readEnd.get(), true, false);
        pipe.fill();
        expected.push_back(pipe.readEnd.get());
    }

    std::sort(expected.begin(), expected.end());
    ASSERT_EQ(readableFds(this->poller), expected);

    // First, middle, last, then the rest.
    for (std::size_t index : {0u, 2u, 4u, 1u, 3u}) {
        int fd = pipes[index].readEnd.get();

        this->poller.remove(fd);
        expected.erase(std::find(expected.begin(), expected.end(), fd));

        EXPECT_EQ(readableFds(this->poller), expected);
    }
}

TYPED_TEST(PollerTest, ADescriptorCanBeAddedAgainAfterRemoval) {
    Pipe pipe;
    pipe.fill();

    this->poller.add(pipe.readEnd.get(), true, false);
    this->poller.remove(pipe.readEnd.get());
    this->poller.add(pipe.readEnd.get(), true, false);

    EXPECT_EQ(readableFds(this->poller), std::vector<int>{pipe.readEnd.get()});
}

// Linux reports the closed peer as readable (the read returns 0), macOS
// as readable and hangup. Either way the owner is told to look.
TYPED_TEST(PollerTest, ReportsAClosedPeer) {
    SocketPair pair;
    this->poller.add(pair.first.get(), true, false);

    pair.second.reset();
    std::vector<PollEvent> events;

    ASSERT_TRUE(this->poller.wait(0, events));
    ASSERT_EQ(events.size(), 1u);
    EXPECT_TRUE(events[0].readable || events[0].hangup);
}

TYPED_TEST(PollerTest, ReturnsAfterTheTimeout) {
    Pipe pipe;
    this->poller.add(pipe.readEnd.get(), true, false);

    std::vector<PollEvent> events;
    auto start = std::chrono::steady_clock::now();

    EXPECT_TRUE(this->poller.wait(50, events));

    auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_TRUE(events.empty());
    EXPECT_GE(elapsed, std::chrono::milliseconds(40));
}

TYPED_TEST(PollerTest, AddSucceeds) {
    Pipe pipe;

    EXPECT_TRUE(this->poller.add(pipe.readEnd.get(), true, false));
}

TYPED_TEST(PollerTest, SettingTheSameInterestAgainChangesNothing) {
    Pipe pipe;
    this->poller.add(pipe.readEnd.get(), true, false);
    pipe.fill();

    for (int i = 0; i < 100; ++i)
        this->poller.modify(pipe.readEnd.get(), true, false);

    EXPECT_EQ(readableFds(this->poller), std::vector<int>{pipe.readEnd.get()});
}

TYPED_TEST(PollerTest, ReportsReadableAndWritableTogether) {
    SocketPair pair;
    this->poller.add(pair.first.get(), true, true);
    ASSERT_EQ(write(pair.second.get(), "x", 1), 1);

    std::vector<PollEvent> events;

    ASSERT_TRUE(this->poller.wait(0, events));
    ASSERT_EQ(events.size(), 1u);
    EXPECT_TRUE(events[0].readable);
    EXPECT_TRUE(events[0].writable);
}

// A connection that has stopped reading and still has responses to send
// watches only for room to write. A peer that has finished sending must
// not show up as a hangup there, or the server would drop what it owes.
TYPED_TEST(PollerTest, AHalfClosedPeerIsNotAHangupWhileOnlyWriting) {
    SocketPair pair;
    this->poller.add(pair.first.get(), false, true);

    ASSERT_EQ(shutdown(pair.second.get(), SHUT_WR), 0);
    std::vector<PollEvent> events;

    ASSERT_TRUE(this->poller.wait(0, events));
    ASSERT_EQ(events.size(), 1u);
    EXPECT_TRUE(events[0].writable);
    EXPECT_FALSE(events[0].hangup);
    EXPECT_FALSE(events[0].error);
}

// With input wanted, the same half-close has to be noticed.
TYPED_TEST(PollerTest, AHalfClosedPeerIsReportedWhileReading) {
    SocketPair pair;
    this->poller.add(pair.first.get(), true, false);

    ASSERT_EQ(shutdown(pair.second.get(), SHUT_WR), 0);
    std::vector<PollEvent> events;

    ASSERT_TRUE(this->poller.wait(0, events));
    ASSERT_EQ(events.size(), 1u);
    EXPECT_TRUE(events[0].readable || events[0].hangup);
}

// poll() is told about the descriptor on every call, so it can say that
// one is no longer open. epoll forgets a descriptor when it is closed.
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

#ifdef __linux__

// With room for only a few events per wait, the rest must come out of
// the following waits and not be lost.
TEST(EpollPoller, ReportsEverythingEvenWhenMoreIsReadyThanFitsInOneWait) {
    std::vector<Pipe> pipes(50);
    EpollPoller poller(8);
    std::set<int> expected;

    for (Pipe& pipe : pipes) {
        poller.add(pipe.readEnd.get(), true, false);
        pipe.fill();
        expected.insert(pipe.readEnd.get());
    }

    std::set<int> seen;
    std::vector<PollEvent> events;

    for (int round = 0; round < 50 && seen.size() < expected.size(); ++round) {
        ASSERT_TRUE(poller.wait(0, events));
        EXPECT_LE(events.size(), 8u);

        for (const PollEvent& event : events) {
            seen.insert(event.fd);

            // Taken care of, so the next wait reports others.
            poller.modify(event.fd, false, false);
        }
    }

    EXPECT_EQ(seen, expected);
}

TEST(EpollPoller, AddFailsForADescriptorThatIsNotOpen) {
    EpollPoller poller;

    EXPECT_FALSE(poller.add(-1, true, false));
}

TEST(EpollPoller, AddFailsForADescriptorAlreadyAdded) {
    Pipe pipe;
    EpollPoller poller;

    ASSERT_TRUE(poller.add(pipe.readEnd.get(), true, false));
    EXPECT_FALSE(poller.add(pipe.readEnd.get(), true, false));
}

#endif
