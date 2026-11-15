#include "Connection.h"

#include <gtest/gtest.h>

#include <chrono>
#include <csignal>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace protocol;
using std::chrono::milliseconds;

namespace {

Frame makeFrame(std::uint8_t type, std::uint32_t requestId,
                std::string payload = "") {
    Frame frame;
    frame.type = type;
    frame.requestId = requestId;
    frame.payload = std::move(payload);
    return frame;
}

std::string echo(std::uint32_t requestId, std::string payload = "data") {
    return encodeFrame(makeFrame(TYPE_ECHO, requestId, std::move(payload)));
}

// A Connection on one end of a socket pair, with the test playing the
// client on the other end. No server and no event loop are involved.
class ConnectionTest : public ::testing::Test {
protected:
    Config config;
    std::optional<Connection::Limits> limits;
    std::optional<Connection> connection;
    FileDescriptor peer;
    bool peerSawEnd = false;

    Connection::Clock::time_point start = Connection::Clock::now();

    // Writing to a socket whose peer is gone raises SIGPIPE, which would
    // end the test program. The server ignores it in the same way.
    void SetUp() override {
        std::signal(SIGPIPE, SIG_IGN);
    }

    // Tests change config first, then call this.
    void open() {
        int fds[2];
        ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

        for (int fd : fds)
            ASSERT_EQ(fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK), 0);

        peer.reset(fds[1]);
        limits.emplace(config);
        connection.emplace(1, FileDescriptor(fds[0]), *limits, start);
    }

    Connection::Clock::time_point at(int ms) const {
        return start + milliseconds(ms);
    }

    void peerSends(const std::string& bytes) {
        ASSERT_EQ(write(peer.get(), bytes.data(), bytes.size()),
                  static_cast<ssize_t>(bytes.size()));
    }

    void peerFinishesSending() {
        ASSERT_EQ(shutdown(peer.get(), SHUT_WR), 0);
    }

    // Everything the connection has sent so far.
    std::string peerReads() {
        std::string bytes;
        char chunk[65536];

        while (true) {
            ssize_t n = read(peer.get(), chunk, sizeof(chunk));

            if (n > 0) {
                bytes.append(chunk, static_cast<std::size_t>(n));
            }
            else {
                if (n == 0)
                    peerSawEnd = true;

                return bytes;
            }
        }
    }

    // The peer's bytes arrive and the connection takes them.
    void arrive(const std::string& bytes) {
        peerSends(bytes);
        ASSERT_TRUE(connection->receive());
    }

    // Bytes and the peer's FIN are both waiting. The bytes are taken by
    // one call and the FIN is found by the next, as in the server, where
    // the poller reports the socket a second time.
    void receiveBytesThenFin() {
        ASSERT_TRUE(connection->receive());
        ASSERT_TRUE(connection->receive());
    }

    // The next request, or 0 if there is none.
    std::uint32_t nextRequestId() {
        Frame request;
        return connection->nextRequest(request) ? request.requestId : 0;
    }

    // The frames in a byte string, checked to be complete.
    std::vector<Frame> framesIn(const std::string& bytes) {
        std::vector<Frame> frames;
        std::string_view rest = bytes;

        while (!rest.empty()) {
            DecodeResult decoded = decodeFrame(rest, 1 << 24);
            EXPECT_EQ(decoded.status, DecodeStatus::Complete);

            if (decoded.status != DecodeStatus::Complete)
                break;

            rest.remove_prefix(decoded.consumed);
            frames.push_back(std::move(decoded.frame));
        }

        return frames;
    }
};

} // namespace

TEST_F(ConnectionTest, StartsOpenAndIdle) {
    open();

    EXPECT_EQ(connection->state(), State::Open);
    EXPECT_TRUE(connection->wantsRead());
    EXPECT_FALSE(connection->wantsWrite());
    EXPECT_TRUE(connection->owesNothing());
    EXPECT_FALSE(connection->finished());
    EXPECT_EQ(nextRequestId(), 0u);
}

TEST_F(ConnectionTest, YieldsRequestsInTheOrderTheyArrived) {
    open();

    arrive(echo(1) + echo(2) + echo(3));

    EXPECT_EQ(nextRequestId(), 1u);
    EXPECT_EQ(nextRequestId(), 2u);
    EXPECT_EQ(nextRequestId(), 3u);
    EXPECT_EQ(nextRequestId(), 0u);
}

TEST_F(ConnectionTest, WaitsForTheRestOfAPartialFrame) {
    open();
    std::string frame = echo(7, "split in two");

    arrive(frame.substr(0, 9));
    EXPECT_EQ(nextRequestId(), 0u);

    arrive(frame.substr(9));
    EXPECT_EQ(nextRequestId(), 7u);
}

TEST_F(ConnectionTest, ServesNothingElseWhileARequestIsWithAWorker) {
    open();
    arrive(echo(1) + echo(2));

    ASSERT_EQ(nextRequestId(), 1u);
    connection->startWork();

    EXPECT_EQ(nextRequestId(), 0u);
    EXPECT_FALSE(connection->owesNothing());

    connection->finishWork("response");

    EXPECT_EQ(nextRequestId(), 2u);
}

TEST_F(ConnectionTest, SendsWhatWasQueued) {
    open();

    connection->respond("first ");
    connection->respond("second");

    EXPECT_TRUE(connection->wantsWrite());
    ASSERT_TRUE(connection->flush(at(0)));

    EXPECT_FALSE(connection->wantsWrite());
    EXPECT_EQ(peerReads(), "first second");
}

TEST_F(ConnectionTest, AnswersAnInvalidRequestItselfAndGoesOn) {
    open();
    Frame wrongVersion = makeFrame(TYPE_ECHO, 5, "x");
    wrongVersion.version = 2;

    arrive(encodeFrame(wrongVersion) + encodeFrame(makeFrame(0x55, 6)) +
           echo(7));

    EXPECT_EQ(nextRequestId(), 7u);
    EXPECT_EQ(connection->state(), State::Open);

    ASSERT_TRUE(connection->flush(at(0)));
    std::vector<Frame> sent = framesIn(peerReads());

    ASSERT_EQ(sent.size(), 2u);
    EXPECT_EQ(sent[0].type, TYPE_ERROR);
    EXPECT_EQ(sent[0].requestId, 5u);
    EXPECT_EQ(readU16(sent[0].payload.data()), 2);
    EXPECT_EQ(sent[1].type, TYPE_ERROR);
    EXPECT_EQ(sent[1].requestId, 6u);
    EXPECT_EQ(readU16(sent[1].payload.data()), 3);
}

TEST_F(ConnectionTest, ABadLengthEndsTheConversation) {
    open();
    std::string badLength;
    appendU32(badLength, 3);

    arrive(echo(1) + badLength + echo(2));

    EXPECT_EQ(nextRequestId(), 1u);
    EXPECT_EQ(nextRequestId(), 0u);
    EXPECT_EQ(connection->state(), State::Closing);

    // The request behind the bad length is never served.
    EXPECT_EQ(nextRequestId(), 0u);

    ASSERT_TRUE(connection->flush(at(0)));
    std::vector<Frame> sent = framesIn(peerReads());

    ASSERT_EQ(sent.size(), 1u);
    EXPECT_EQ(sent[0].type, TYPE_ERROR);
    EXPECT_EQ(sent[0].requestId, 0u);
    EXPECT_EQ(readU16(sent[0].payload.data()), 1);
}

TEST_F(ConnectionTest, ClosingSendsFinOnlyAfterTheErrorIsOut) {
    open();
    std::string badLength;
    appendU32(badLength, 0);
    arrive(badLength);
    ASSERT_EQ(nextRequestId(), 0u);

    // The error is still in the output buffer.
    connection->sendFinIfDue();
    peerReads();
    EXPECT_FALSE(peerSawEnd);

    ASSERT_TRUE(connection->flush(at(0)));
    connection->sendFinIfDue();

    EXPECT_EQ(framesIn(peerReads()).size(), 1u);
    EXPECT_TRUE(peerSawEnd);
}

TEST_F(ConnectionTest, ClosingKeepsReadingAndDropsWhatArrives) {
    open();
    std::string badLength;
    appendU32(badLength, 0);
    arrive(badLength);
    ASSERT_EQ(nextRequestId(), 0u);
    ASSERT_TRUE(connection->flush(at(0)));
    connection->sendFinIfDue();
    peerReads();

    EXPECT_TRUE(connection->wantsRead());

    arrive(echo(1) + echo(2));

    EXPECT_EQ(nextRequestId(), 0u);
    EXPECT_FALSE(connection->wantsWrite());
    EXPECT_FALSE(connection->finished());

    // The peer closing is what ends it.
    peerFinishesSending();
    ASSERT_TRUE(connection->receive());

    EXPECT_TRUE(connection->finished());
}

TEST_F(ConnectionTest, AfterThePeersFinRequestsAlreadyReceivedAreStillServed) {
    open();
    peerSends(echo(1) + echo(2));
    peerFinishesSending();
    receiveBytesThenFin();

    EXPECT_EQ(connection->state(), State::ReadClosed);
    EXPECT_FALSE(connection->wantsRead());
    EXPECT_FALSE(connection->finished());

    ASSERT_EQ(nextRequestId(), 1u);
    connection->startWork();
    EXPECT_FALSE(connection->finished());

    connection->finishWork("one");
    ASSERT_EQ(nextRequestId(), 2u);
    connection->respond("two");
    EXPECT_EQ(nextRequestId(), 0u);

    // Still owes the two responses.
    EXPECT_FALSE(connection->finished());

    ASSERT_TRUE(connection->flush(at(0)));

    EXPECT_TRUE(connection->finished());
    EXPECT_EQ(peerReads(), "onetwo");
}

TEST_F(ConnectionTest, AFrameCutOffByThePeersFinIsDropped) {
    open();
    peerSends(echo(1).substr(0, 10));
    peerFinishesSending();
    receiveBytesThenFin();

    EXPECT_FALSE(connection->finished());
    EXPECT_EQ(nextRequestId(), 0u);
    EXPECT_TRUE(connection->finished());
}

TEST_F(ConnectionTest, ABadLengthAfterThePeersFinStaysReadClosed) {
    open();
    std::string badLength;
    appendU32(badLength, 5);
    peerSends(badLength);
    peerFinishesSending();
    receiveBytesThenFin();

    EXPECT_EQ(nextRequestId(), 0u);
    EXPECT_EQ(connection->state(), State::ReadClosed);
    EXPECT_FALSE(connection->finished());

    ASSERT_TRUE(connection->flush(at(0)));

    EXPECT_TRUE(connection->finished());
}

TEST_F(ConnectionTest, FlushFailsOnceThePeerIsGone) {
    open();
    connection->respond(std::string(1 << 20, 'x'));
    peer.reset();

    // Writing to a socket whose peer is gone is an error.
    EXPECT_FALSE(connection->flush(at(0)));
}

// Throttling starts exactly at the high watermark.
TEST_F(ConnectionTest, StopsReadingAndServingAtTheHighWatermark) {
    config.outputHighWater = 1000;
    open();
    arrive(echo(1));

    connection->respond(std::string(999, 'a'));
    EXPECT_TRUE(connection->wantsRead());

    connection->respond("b");

    EXPECT_FALSE(connection->wantsRead());
    EXPECT_EQ(nextRequestId(), 0u);
}

TEST_F(ConnectionTest, StaysThrottledUntilTheOutputHasDrained) {
    // Far more than the socket buffers hold, so one flush cannot send
    // it all.
    config.outputHighWater = 4 << 20;
    open();
    arrive(echo(1));
    connection->respond(std::string(8 << 20, 'x'));

    ASSERT_TRUE(connection->flush(at(0)));

    EXPECT_TRUE(connection->wantsWrite());
    EXPECT_FALSE(connection->wantsRead());
    EXPECT_EQ(nextRequestId(), 0u);

    std::size_t received = 0;

    while (connection->wantsWrite()) {
        received += peerReads().size();
        ASSERT_TRUE(connection->flush(at(0)));
    }

    received += peerReads().size();

    EXPECT_EQ(received, std::size_t{8} << 20);
    EXPECT_TRUE(connection->wantsRead());
    EXPECT_EQ(nextRequestId(), 1u);
}

// One frame of the largest size is all the input buffer holds.
TEST_F(ConnectionTest, StopsReadingWhenTheInputBufferIsFull) {
    config.maxFrame = 64;
    open();
    std::string frame = echo(1, std::string(56, 'p'));
    ASSERT_EQ(frame.size(), 68u);

    arrive(frame + frame.substr(0, 30));

    // 68 bytes were taken. The other 30 stay in the socket.
    EXPECT_FALSE(connection->wantsRead());

    ASSERT_EQ(nextRequestId(), 1u);
    EXPECT_TRUE(connection->wantsRead());

    ASSERT_TRUE(connection->receive());
    peerSends(frame.substr(30));
    ASSERT_TRUE(connection->receive());

    EXPECT_EQ(nextRequestId(), 1u);
}

TEST_F(ConnectionTest, DrainDropsRequestsThatWereNotStarted) {
    open();
    arrive(echo(1) + echo(2));
    ASSERT_EQ(nextRequestId(), 1u);
    connection->startWork();

    connection->drain();

    EXPECT_EQ(connection->state(), State::Closing);
    EXPECT_FALSE(connection->owesNothing());

    connection->finishWork("answer");

    EXPECT_EQ(nextRequestId(), 0u);

    ASSERT_TRUE(connection->flush(at(0)));
    connection->sendFinIfDue();

    EXPECT_EQ(peerReads(), "answer");
    EXPECT_TRUE(peerSawEnd);
}

TEST_F(ConnectionTest, DrainDoesNotSendFinWhileAWorkerHasTheRequest) {
    open();
    arrive(echo(1));
    ASSERT_EQ(nextRequestId(), 1u);
    connection->startWork();
    connection->drain();

    connection->sendFinIfDue();
    peerReads();

    EXPECT_FALSE(peerSawEnd);
}

TEST_F(ConnectionTest, AnIdleConnectionOwesNothingWhenDrained) {
    open();
    arrive(echo(1).substr(0, 5));

    connection->drain();

    EXPECT_TRUE(connection->owesNothing());
}

TEST_F(ConnectionTest, IdleRunsOutAfterTheIdleTimeout) {
    config.idleTimeoutMs = 1000;
    open();

    EXPECT_EQ(connection->timeLeftMs(at(0)), 1000);
    EXPECT_EQ(connection->phase(), Phase::Idle);
    EXPECT_EQ(connection->timeLeftMs(at(400)), 600);
    EXPECT_EQ(connection->timeLeftMs(at(1000)), 0);
    EXPECT_EQ(connection->timeLeftMs(at(5000)), 0);
}

// More bytes of the same frame are activity, but they do not restart
// the clock. Only finishing the frame does.
TEST_F(ConnectionTest, APartialFrameIsTimedFromItsFirstByte) {
    config.partialFrameTimeoutMs = 1000;
    config.idleTimeoutMs = 9000;
    open();
    std::string frame = echo(1, "slowly");
    connection->timeLeftMs(at(0));

    arrive(frame.substr(0, 3));
    ASSERT_EQ(nextRequestId(), 0u);

    EXPECT_EQ(connection->timeLeftMs(at(100)), 1000);
    EXPECT_EQ(connection->phase(), Phase::Partial);

    arrive(frame.substr(3, 3));
    ASSERT_EQ(nextRequestId(), 0u);

    EXPECT_EQ(connection->timeLeftMs(at(700)), 400);
    EXPECT_EQ(connection->timeLeftMs(at(1100)), 0);
}

TEST_F(ConnectionTest, FinishingAFrameEndsThePartialPhase) {
    config.partialFrameTimeoutMs = 1000;
    config.idleTimeoutMs = 9000;
    open();
    std::string frame = echo(1);

    arrive(frame.substr(0, 3));
    ASSERT_EQ(nextRequestId(), 0u);
    connection->timeLeftMs(at(0));

    arrive(frame.substr(3));
    ASSERT_EQ(nextRequestId(), 1u);

    // Answered without a worker, and the answer sent at once.
    connection->respond("ok");
    ASSERT_TRUE(connection->flush(at(900)));

    EXPECT_EQ(connection->timeLeftMs(at(900)), 9000);
    EXPECT_EQ(connection->phase(), Phase::Idle);
}

TEST_F(ConnectionTest, ARequestWithAWorkerHasNoTimeLimit) {
    open();
    arrive(echo(1));
    ASSERT_EQ(nextRequestId(), 1u);
    connection->startWork();

    EXPECT_EQ(connection->timeLeftMs(at(0)), -1);
    EXPECT_EQ(connection->phase(), Phase::Busy);
    EXPECT_EQ(connection->timeLeftMs(at(10000000)), -1);
}

// The limit is on a stall, not on how long a large response takes.
TEST_F(ConnectionTest, SendingOutputRestartsTheStallClock) {
    config.writeStallTimeoutMs = 1000;
    config.outputHighWater = 64 << 20;
    open();
    connection->respond(std::string(8 << 20, 'x'));

    EXPECT_EQ(connection->timeLeftMs(at(0)), 1000);
    EXPECT_EQ(connection->phase(), Phase::Writing);

    // The socket buffer takes the first part.
    ASSERT_TRUE(connection->flush(at(100)));
    EXPECT_EQ(connection->timeLeftMs(at(100)), 1000);

    // It is full now. Nothing moves, so the clock keeps running.
    ASSERT_TRUE(connection->flush(at(600)));
    EXPECT_EQ(connection->timeLeftMs(at(600)), 500);

    // The peer reads, output moves again, and the clock starts over.
    peerReads();
    ASSERT_TRUE(connection->flush(at(900)));
    EXPECT_EQ(connection->timeLeftMs(at(900)), 1000);

    EXPECT_EQ(connection->timeLeftMs(at(1900)), 0);
}

TEST_F(ConnectionTest, ClosingLingersOnlyAfterTheOutputIsSent) {
    config.writeStallTimeoutMs = 5000;
    config.lingerTimeoutMs = 1000;
    open();
    std::string badLength;
    appendU32(badLength, 0);
    arrive(badLength);
    ASSERT_EQ(nextRequestId(), 0u);

    EXPECT_EQ(connection->timeLeftMs(at(0)), 5000);
    EXPECT_EQ(connection->phase(), Phase::Writing);

    ASSERT_TRUE(connection->flush(at(200)));
    connection->sendFinIfDue();

    EXPECT_EQ(connection->timeLeftMs(at(200)), 1000);
    EXPECT_EQ(connection->phase(), Phase::Lingering);
    EXPECT_EQ(connection->timeLeftMs(at(1200)), 0);
}

// During a shutdown a request may still be with a worker. That is not
// lingering, and it must not be cut off by the linger limit.
TEST_F(ConnectionTest, ADrainingConnectionWithWorkInFlightIsBusy) {
    config.lingerTimeoutMs = 1000;
    open();
    arrive(echo(1));
    ASSERT_EQ(nextRequestId(), 1u);
    connection->startWork();
    connection->drain();

    EXPECT_EQ(connection->timeLeftMs(at(0)), -1);
    EXPECT_EQ(connection->phase(), Phase::Busy);
}
