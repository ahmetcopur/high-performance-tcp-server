#include "Buffer.h"
#include "Protocol.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

using namespace protocol;

namespace {

constexpr std::uint32_t MAX_LENGTH = 1 << 20;

Frame makeFrame(std::uint8_t type, std::uint32_t requestId,
                std::string payload) {
    Frame frame;
    frame.type = type;
    frame.requestId = requestId;
    frame.payload = std::move(payload);
    return frame;
}

void expectSame(const Frame& actual, const Frame& expected) {
    EXPECT_EQ(actual.version, expected.version);
    EXPECT_EQ(actual.type, expected.type);
    EXPECT_EQ(actual.reserved, expected.reserved);
    EXPECT_EQ(actual.requestId, expected.requestId);
    EXPECT_EQ(actual.payload, expected.payload);
}

std::string lengthOnly(std::uint32_t length) {
    std::string bytes;
    appendU32(bytes, length);
    return bytes;
}

bool validates(const Frame& frame) {
    ErrorCode error;
    return validateRequest(frame, error);
}

ErrorCode rejection(const Frame& frame) {
    ErrorCode error = ErrorCode::InternalError;
    EXPECT_FALSE(validateRequest(frame, error));
    return error;
}

} // namespace

TEST(ByteOrder, WritesMostSignificantByteFirst) {
    std::string bytes;

    appendU16(bytes, 0x0102);
    appendU32(bytes, 0x03040506);
    appendU64(bytes, 0x0708090a0b0c0d0eull);

    EXPECT_EQ(bytes, std::string("\x01\x02\x03\x04\x05\x06\x07\x08"
                                 "\x09\x0a\x0b\x0c\x0d\x0e",
                                 14));
}

// Bytes above 0x7f are negative as plain char. They must not sign-extend.
TEST(ByteOrder, ReadsBackValuesWithHighBitsSet) {
    std::string bytes;

    appendU16(bytes, 0xfffe);
    appendU32(bytes, 0xfffefdfc);
    appendU64(bytes, 0xfffefdfcfbfaf9f8ull);

    EXPECT_EQ(readU16(bytes.data()), 0xfffe);
    EXPECT_EQ(readU32(bytes.data() + 2), 0xfffefdfcu);
    EXPECT_EQ(readU64(bytes.data() + 6), 0xfffefdfcfbfaf9f8ull);
}

// The worked example in docs/protocol.md.
TEST(Encode, ProducesTheDocumentedBytes) {
    std::string bytes = encodeFrame(makeFrame(TYPE_ECHO, 7, "hi"));

    EXPECT_EQ(bytes, std::string("\x00\x00\x00\x0a" // length 10
                                 "\x01"             // version
                                 "\x02"             // type ECHO
                                 "\x00\x00"         // reserved
                                 "\x00\x00\x00\x07" // request id
                                 "hi",
                                 14));
}

TEST(Encode, ErrorPayloadIsCodeThenMessage) {
    std::string bytes = encodeError(9, ErrorCode::UnknownType,
                                    errorName(ErrorCode::UnknownType));

    DecodeResult result = decodeFrame(bytes, MAX_LENGTH);

    ASSERT_EQ(result.status, DecodeStatus::Complete);
    EXPECT_EQ(result.frame.type, TYPE_ERROR);
    EXPECT_EQ(result.frame.requestId, 9u);
    ASSERT_GE(result.frame.payload.size(), 2u);
    EXPECT_EQ(readU16(result.frame.payload.data()), 3);
    EXPECT_EQ(result.frame.payload.substr(2), "unknown type");
}

TEST(Decode, RoundTripsEveryKindOfFrame) {
    std::string binary("\x00\xff\x00\x80\x7f", 5);

    std::vector<Frame> frames = {
        makeFrame(TYPE_PING, 0, ""),
        makeFrame(TYPE_ECHO, 1, "hello"),
        makeFrame(TYPE_COMPUTE, 0xffffffffu, binary),
        makeFrame(TYPE_ECHO | RESPONSE_BIT, 0x80000000u,
                  std::string(70000, 'x')),
        makeFrame(TYPE_ERROR, 42, std::string("\x00\x01oops", 6)),
    };

    for (const Frame& frame : frames) {
        std::string bytes = encodeFrame(frame);

        DecodeResult result = decodeFrame(bytes, MAX_LENGTH);

        ASSERT_EQ(result.status, DecodeStatus::Complete);
        EXPECT_EQ(result.consumed, bytes.size());
        expectSame(result.frame, frame);
    }
}

// Version, type and reserved do not decide where a frame ends, so the
// decoder passes them through untouched.
TEST(Decode, DoesNotJudgeVersionTypeOrReserved) {
    Frame odd = makeFrame(0x77, 5, "payload");
    odd.version = 9;
    odd.reserved = 0x1234;

    DecodeResult result = decodeFrame(encodeFrame(odd), MAX_LENGTH);

    ASSERT_EQ(result.status, DecodeStatus::Complete);
    expectSame(result.frame, odd);
}

TEST(Decode, IncompleteUntilTheWholeFrameIsThere) {
    std::string bytes = encodeFrame(makeFrame(TYPE_ECHO, 3, "abcdef"));

    for (std::size_t size = 0; size < bytes.size(); ++size) {
        DecodeResult result = decodeFrame(bytes.substr(0, size), MAX_LENGTH);

        EXPECT_EQ(result.status, DecodeStatus::Incomplete) << size << " bytes";
        EXPECT_EQ(result.consumed, 0u);
    }
}

TEST(Decode, OneByteAtATime) {
    Frame frame = makeFrame(TYPE_ECHO, 11, "one byte at a time");
    std::string bytes = encodeFrame(frame);
    Buffer buffer;

    for (std::size_t i = 0; i + 1 < bytes.size(); ++i) {
        buffer.append(bytes.substr(i, 1));

        ASSERT_EQ(decodeFrame(buffer.view(), MAX_LENGTH).status,
                  DecodeStatus::Incomplete);
    }

    buffer.append(bytes.substr(bytes.size() - 1));
    DecodeResult result = decodeFrame(buffer.view(), MAX_LENGTH);

    ASSERT_EQ(result.status, DecodeStatus::Complete);
    expectSame(result.frame, frame);
}

// Two frames back to back, cut in two pieces at every possible place.
// The cut falls inside the first header, its payload, on the boundary,
// and inside the second frame.
TEST(Decode, SplitAtEveryOffset) {
    Frame first = makeFrame(TYPE_ECHO, 1, "first payload");
    Frame second = makeFrame(TYPE_COMPUTE, 2, "second");
    std::string bytes = encodeFrame(first) + encodeFrame(second);

    for (std::size_t cut = 0; cut <= bytes.size(); ++cut) {
        Buffer buffer;
        std::vector<Frame> decoded;

        for (std::string piece : {bytes.substr(0, cut), bytes.substr(cut)}) {
            buffer.append(piece);

            while (true) {
                DecodeResult result = decodeFrame(buffer.view(), MAX_LENGTH);

                ASSERT_NE(result.status, DecodeStatus::Invalid);

                if (result.status == DecodeStatus::Incomplete)
                    break;

                buffer.consume(result.consumed);
                decoded.push_back(std::move(result.frame));
            }
        }

        ASSERT_EQ(decoded.size(), 2u) << "cut at " << cut;
        expectSame(decoded[0], first);
        expectSame(decoded[1], second);
        EXPECT_EQ(buffer.size(), 0u);
    }
}

TEST(Decode, ManyFramesInOneBuffer) {
    Buffer buffer;

    for (std::uint32_t i = 0; i < 500; ++i)
        buffer.append(
            encodeFrame(makeFrame(TYPE_ECHO, i, std::string(i % 50, 'a'))));

    std::string partial = encodeFrame(makeFrame(TYPE_PING, 500, "tail"));
    partial.pop_back();
    buffer.append(partial);

    for (std::uint32_t i = 0; i < 500; ++i) {
        DecodeResult result = decodeFrame(buffer.view(), MAX_LENGTH);

        ASSERT_EQ(result.status, DecodeStatus::Complete);
        EXPECT_EQ(result.frame.requestId, i);
        EXPECT_EQ(result.frame.payload, std::string(i % 50, 'a'));

        buffer.consume(result.consumed);
    }

    EXPECT_EQ(decodeFrame(buffer.view(), MAX_LENGTH).status,
              DecodeStatus::Incomplete);
    EXPECT_EQ(buffer.view(), partial);
}

TEST(Decode, LengthBelowMinimumIsInvalid) {
    for (std::uint32_t length = 0; length < MIN_LENGTH; ++length) {
        DecodeResult result = decodeFrame(lengthOnly(length), MAX_LENGTH);

        EXPECT_EQ(result.status, DecodeStatus::Invalid) << "length " << length;
        EXPECT_EQ(result.error, ErrorCode::MalformedFrame);
    }
}

// Refused from the length field alone, before any payload arrives.
TEST(Decode, LengthAboveMaximumIsInvalid) {
    for (std::uint32_t length : {MAX_LENGTH + 1, 0x7fffffffu, 0xffffffffu}) {
        DecodeResult result = decodeFrame(lengthOnly(length), MAX_LENGTH);

        EXPECT_EQ(result.status, DecodeStatus::Invalid) << "length " << length;
        EXPECT_EQ(result.error, ErrorCode::PayloadTooLarge);
    }
}

TEST(Decode, AcceptsAFrameOfExactlyTheMaximumLength) {
    Frame frame =
        makeFrame(TYPE_ECHO, 1, std::string(MAX_LENGTH - MIN_LENGTH, 'm'));
    std::string bytes = encodeFrame(frame);

    ASSERT_EQ(readU32(bytes.data()), MAX_LENGTH);

    DecodeResult result = decodeFrame(bytes, MAX_LENGTH);

    ASSERT_EQ(result.status, DecodeStatus::Complete);
    EXPECT_EQ(result.frame.payload.size(), MAX_LENGTH - MIN_LENGTH);

    bytes = encodeFrame(
        makeFrame(TYPE_ECHO, 1, std::string(MAX_LENGTH - MIN_LENGTH + 1, 'm')));

    EXPECT_EQ(decodeFrame(bytes, MAX_LENGTH).status, DecodeStatus::Invalid);
}

TEST(Decode, AcceptsTheSmallestFrame) {
    DecodeResult result =
        decodeFrame(encodeFrame(makeFrame(TYPE_PING, 1, "")), MIN_LENGTH);

    ASSERT_EQ(result.status, DecodeStatus::Complete);
    EXPECT_EQ(result.consumed, HEADER_SIZE);
    EXPECT_EQ(result.frame.payload, "");
}

TEST(Validate, AcceptsTheRequestTypes) {
    EXPECT_TRUE(validates(makeFrame(TYPE_PING, 1, "")));
    EXPECT_TRUE(validates(makeFrame(TYPE_ECHO, 1, "x")));
    EXPECT_TRUE(validates(makeFrame(TYPE_COMPUTE, 1, "x")));
    EXPECT_TRUE(validates(makeFrame(TYPE_STATS, 1, "")));
}

TEST(Validate, RejectsOtherVersions) {
    for (int version : {0, 2, 255}) {
        Frame frame = makeFrame(TYPE_ECHO, 1, "x");
        frame.version = static_cast<std::uint8_t>(version);

        EXPECT_EQ(rejection(frame), ErrorCode::UnsupportedVersion);
    }
}

TEST(Validate, RejectsNonzeroReserved) {
    Frame frame = makeFrame(TYPE_ECHO, 1, "x");
    frame.reserved = 1;

    EXPECT_EQ(rejection(frame), ErrorCode::MalformedFrame);
}

// Response and error types are not requests.
TEST(Validate, RejectsTypesThatAreNotRequests) {
    for (int type : {0, 5, 0x7f, 0x81, 0x82, 0x84, 0xff})
        EXPECT_EQ(rejection(makeFrame(static_cast<std::uint8_t>(type), 1, "")),
                  ErrorCode::UnknownType)
            << "type " << type;
}

TEST(Validate, ReportsTheVersionBeforeAnythingElse) {
    Frame frame = makeFrame(0x55, 1, "");
    frame.version = 2;
    frame.reserved = 7;

    EXPECT_EQ(rejection(frame), ErrorCode::UnsupportedVersion);
}

// Published 64-bit FNV-1a test vectors.
TEST(ComputeHash, MatchesFnv1aForOnePass) {
    EXPECT_EQ(computeHash("", 1), 0xcbf29ce484222325ull);
    EXPECT_EQ(computeHash("a", 1), 0xaf63dc4c8601ec8cull);
    EXPECT_EQ(computeHash("foobar", 1), 0x85944171f73967e8ull);
}

// The COMPUTE example in docs/protocol.md.
TEST(ComputeHash, MatchesTheDocumentedExample) {
    EXPECT_EQ(computeHash("abc", 3), 0xd328b56182a25b1bull);
}

TEST(ComputeHash, ZeroIterationsLeavesTheStartingValue) {
    EXPECT_EQ(computeHash("anything", 0), 0xcbf29ce484222325ull);
}

TEST(ComputeHash, PassesContinueFromThePreviousOne) {
    EXPECT_EQ(computeHash("ab", 3), computeHash("ababab", 1));
    EXPECT_NE(computeHash("ab", 3), computeHash("ab", 1));
}

TEST(ComputeHash, TreatsBytesAsUnsigned) {
    EXPECT_EQ(computeHash(std::string("\xff", 1), 1), 0xaf64724c8602eb6eull);
}

TEST(ComputeHash, AFlagThatStaysFalseChangesNothing) {
    std::atomic<bool> cancelled{false};

    EXPECT_EQ(computeHash("abc", 3, &cancelled), computeHash("abc", 3));
}

// Uncancelled, this is 100,000 passes over a megabyte and takes minutes.
TEST(ComputeHash, StopsAtOnceWhenCancelled) {
    std::atomic<bool> cancelled{true};
    std::string data(1 << 20, 'x');

    computeHash(data, MAX_COMPUTE_ITERATIONS, &cancelled);
}

TEST(Stats, AreTenBigEndianIntegersInTheDocumentedOrder) {
    ServerStats stats;
    stats.connections = 1;
    stats.queueDepth = 2;
    stats.queueCapacity = 3;
    stats.workers = 4;
    stats.tasksSubmitted = 5;
    stats.tasksRejected = 6;
    stats.tasksCompleted = 7;
    stats.tasksFailed = 8;
    stats.workerBusyMicroseconds = 9;
    stats.uptimeMicroseconds = 0x0102030405060708ull;

    std::string payload = encodeStats(stats);

    ASSERT_EQ(payload.size(), STATS_SIZE);

    for (std::uint64_t field = 0; field < 9; ++field)
        EXPECT_EQ(readU64(payload.data() + field * 8), field + 1);

    EXPECT_EQ(payload.substr(72),
              std::string("\x01\x02\x03\x04\x05\x06\x07\x08", 8));
}

TEST(Stats, RoundTrip) {
    ServerStats stats;
    stats.connections = 12;
    stats.queueDepth = 34;
    stats.queueCapacity = 1024;
    stats.workers = 8;
    stats.tasksSubmitted = 0xffffffffffffffffull;
    stats.tasksRejected = 56;
    stats.tasksCompleted = 78;
    stats.tasksFailed = 9;
    stats.workerBusyMicroseconds = 123456789;
    stats.uptimeMicroseconds = 987654321;

    ServerStats back;
    ASSERT_TRUE(decodeStats(encodeStats(stats), back));

    EXPECT_EQ(back.connections, 12u);
    EXPECT_EQ(back.queueDepth, 34u);
    EXPECT_EQ(back.queueCapacity, 1024u);
    EXPECT_EQ(back.workers, 8u);
    EXPECT_EQ(back.tasksSubmitted, 0xffffffffffffffffull);
    EXPECT_EQ(back.tasksRejected, 56u);
    EXPECT_EQ(back.tasksCompleted, 78u);
    EXPECT_EQ(back.tasksFailed, 9u);
    EXPECT_EQ(back.workerBusyMicroseconds, 123456789u);
    EXPECT_EQ(back.uptimeMicroseconds, 987654321u);
}

// Fields may be added at the end later.
TEST(Stats, DecodeAcceptsALongerPayloadAndRefusesAShorterOne) {
    ServerStats stats;
    stats.workers = 4;
    std::string payload = encodeStats(stats);

    ServerStats back;
    EXPECT_TRUE(decodeStats(payload + "extra", back));
    EXPECT_EQ(back.workers, 4u);

    EXPECT_FALSE(decodeStats(payload.substr(0, STATS_SIZE - 1), back));
    EXPECT_FALSE(decodeStats("", back));
}

// The STATS and ERROR 5 examples in docs/protocol.md.
TEST(Stats, MatchTheDocumentedExample) {
    ServerStats stats;
    stats.connections = 3;
    stats.queueCapacity = 1024;
    stats.workers = 8;
    stats.tasksSubmitted = 120;
    stats.tasksCompleted = 120;
    stats.workerBusyMicroseconds = 45000;
    stats.uptimeMicroseconds = 60000000;

    Frame response =
        makeFrame(TYPE_STATS | RESPONSE_BIT, 5, encodeStats(stats));
    std::string bytes = encodeFrame(response);

    EXPECT_EQ(
        bytes.substr(0, 12),
        std::string("\x00\x00\x00\x58\x01\x84\x00\x00\x00\x00\x00\x05", 12));
    EXPECT_EQ(bytes.substr(12 + 16, 8),
              std::string("\x00\x00\x00\x00\x00\x00\x04\x00", 8));
    EXPECT_EQ(bytes.substr(12 + 64, 8),
              std::string("\x00\x00\x00\x00\x00\x00\xaf\xc8", 8));
    EXPECT_EQ(bytes.substr(12 + 72, 8),
              std::string("\x00\x00\x00\x00\x03\x93\x87\x00", 8));
}

TEST(Encode, ServerBusyMatchesTheDocumentedExample) {
    std::string bytes =
        encodeError(7, ErrorCode::ServerBusy, errorName(ErrorCode::ServerBusy));

    EXPECT_EQ(bytes, std::string("\x00\x00\x00\x15\x01\xff\x00\x00"
                                 "\x00\x00\x00\x07\x00\x05"
                                 "server busy",
                                 25));
}
