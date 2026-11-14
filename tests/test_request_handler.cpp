#include "RequestHandler.h"

#include <gtest/gtest.h>

#include <atomic>
#include <stdexcept>
#include <string>

using namespace protocol;

namespace {

const std::atomic<bool> KEEP_GOING{false};

Frame request(std::uint8_t type, std::uint32_t requestId, std::string payload) {
    Frame frame;
    frame.type = type;
    frame.requestId = requestId;
    frame.payload = std::move(payload);
    return frame;
}

// The single frame in an encoded response.
Frame decoded(const std::string& bytes) {
    DecodeResult result = decodeFrame(bytes, 1 << 24);

    EXPECT_EQ(result.status, DecodeStatus::Complete);
    EXPECT_EQ(result.consumed, bytes.size());

    return result.frame;
}

std::string computePayload(std::uint32_t iterations, const std::string& data) {
    std::string payload;
    appendU32(payload, iterations);
    payload += data;
    return payload;
}

} // namespace

TEST(ProcessRequest, EchoReturnsThePayloadUnderTheSameId) {
    std::string payload("with\0zero", 9);

    Frame response =
        decoded(processRequest(request(TYPE_ECHO, 17, payload), KEEP_GOING));

    EXPECT_EQ(response.type, TYPE_ECHO | RESPONSE_BIT);
    EXPECT_EQ(response.requestId, 17u);
    EXPECT_EQ(response.payload, payload);
}

TEST(ProcessRequest, ComputeReturnsTheHash) {
    Frame response = decoded(processRequest(
        request(TYPE_COMPUTE, 8, computePayload(3, "abc")), KEEP_GOING));

    EXPECT_EQ(response.type, TYPE_COMPUTE | RESPONSE_BIT);
    EXPECT_EQ(response.requestId, 8u);
    ASSERT_EQ(response.payload.size(), 8u);
    EXPECT_EQ(readU64(response.payload.data()), 0xd328b56182a25b1bull);
}

TEST(ProcessRequest, ComputeAcceptsEmptyDataAndZeroIterations) {
    Frame response = decoded(processRequest(
        request(TYPE_COMPUTE, 1, computePayload(0, "")), KEEP_GOING));

    EXPECT_EQ(response.type, TYPE_COMPUTE | RESPONSE_BIT);
    EXPECT_EQ(readU64(response.payload.data()), 0xcbf29ce484222325ull);
}

TEST(ProcessRequest, ComputeRejectsAPayloadWithoutAnIterationCount) {
    for (const char* payload : {"", "a", "abc"}) {
        Frame response = decoded(
            processRequest(request(TYPE_COMPUTE, 5, payload), KEEP_GOING));

        EXPECT_EQ(response.type, TYPE_ERROR);
        EXPECT_EQ(response.requestId, 5u);
        EXPECT_EQ(readU16(response.payload.data()), 7);
    }
}

TEST(ProcessRequest, ComputeRejectsTooManyIterations) {
    Frame allowed = decoded(processRequest(
        request(TYPE_COMPUTE, 1, computePayload(MAX_COMPUTE_ITERATIONS, "")),
        KEEP_GOING));
    Frame refused = decoded(
        processRequest(request(TYPE_COMPUTE, 2,
                               computePayload(MAX_COMPUTE_ITERATIONS + 1, "")),
                       KEEP_GOING));

    EXPECT_EQ(allowed.type, TYPE_COMPUTE | RESPONSE_BIT);
    EXPECT_EQ(refused.type, TYPE_ERROR);
    EXPECT_EQ(refused.requestId, 2u);
    EXPECT_EQ(readU16(refused.payload.data()), 7);
}

// 100,000 passes over a megabyte would take minutes.
TEST(ProcessRequest, ComputeGivesUpWhenToldToStop) {
    std::atomic<bool> stop{true};

    processRequest(request(TYPE_COMPUTE, 1,
                           computePayload(MAX_COMPUTE_ITERATIONS,
                                          std::string(1 << 20, 'x'))),
                   stop);
}

TEST(AnswerOrInternalError, PassesAnAnswerThrough) {
    std::string answer = answerOrInternalError(3, []() {
        return std::string("the answer");
    });

    EXPECT_EQ(answer, "the answer");
}

TEST(AnswerOrInternalError, TurnsAnExceptionIntoError6ForThatRequest) {
    Frame response = decoded(answerOrInternalError(41, []() -> std::string {
        throw std::runtime_error("something broke");
    }));

    EXPECT_EQ(response.type, TYPE_ERROR);
    EXPECT_EQ(response.requestId, 41u);
    ASSERT_GE(response.payload.size(), 2u);
    EXPECT_EQ(readU16(response.payload.data()), 6);
    EXPECT_EQ(response.payload.substr(2), "internal error");
}

TEST(AnswerOrInternalError, CatchesThingsThatAreNotStdExceptions) {
    Frame response = decoded(answerOrInternalError(42, []() -> std::string {
        throw 7;
    }));

    EXPECT_EQ(response.type, TYPE_ERROR);
    EXPECT_EQ(response.requestId, 42u);
    EXPECT_EQ(readU16(response.payload.data()), 6);
}
