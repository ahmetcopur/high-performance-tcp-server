// Fuzz target for the frame decoder and the functions around it.
//
// libFuzzer calls LLVMFuzzerTestOneInput() over and over with bytes it
// makes up, keeping the inputs that reach new code. A crash, a sanitizer
// report or a failed check below stops it and saves the input.
//
// Crashes are one thing it finds. The checks give it more to find: for
// every input, what the decoder says must agree with what the bytes are.
//
//   cmake -B build-fuzz -DENABLE_FUZZING=ON -DCMAKE_CXX_COMPILER=clang++
//   cmake --build build-fuzz
//   build-fuzz/fuzz_decode -max_total_time=60 tests/fuzz/corpus/decode

#include "Protocol.h"
#include "RequestHandler.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

using namespace protocol;

namespace {

// Small, so that the fuzzer can find its way past it.
constexpr std::uint32_t MAX_LENGTH = 1024;

void require(bool condition) {
    if (!condition)
        std::abort();
}

// A response must be one whole frame and nothing else.
Frame decodeWhole(const std::string& bytes) {
    DecodeResult decoded = decodeFrame(bytes, 1 << 20);

    require(decoded.status == DecodeStatus::Complete);
    require(decoded.consumed == bytes.size());

    return decoded.frame;
}

void checkRequest(Frame frame) {
    ErrorCode error{};
    bool valid = validateRequest(frame, error);

    if (valid) {
        require(frame.version == VERSION);
        require(frame.reserved == 0);
        require(frame.type >= TYPE_PING && frame.type <= TYPE_STATS);
    }
    else {
        require(error == ErrorCode::UnsupportedVersion ||
                error == ErrorCode::MalformedFrame ||
                error == ErrorCode::UnknownType);
    }

    ServerStats stats;

    if (decodeStats(frame.payload, stats)) {
        require(frame.payload.size() >= STATS_SIZE);
        require(encodeStats(stats) == frame.payload.substr(0, STATS_SIZE));
    }
    else {
        require(frame.payload.size() < STATS_SIZE);
    }

    // What a worker does with the request. COMPUTE is only run when it
    // is quick, or refused outright: the fuzzer has no use for an input
    // that takes a second.
    if (!valid || (frame.type != TYPE_ECHO && frame.type != TYPE_COMPUTE))
        return;

    if (frame.type == TYPE_COMPUTE && frame.payload.size() >= 4) {
        std::uint32_t iterations = readU32(frame.payload.data());

        if (iterations > 64 && iterations <= MAX_COMPUTE_ITERATIONS)
            return;
    }

    std::atomic<bool> stopWork{false};
    std::uint8_t type = frame.type;
    std::uint32_t requestId = frame.requestId;
    std::string payload = frame.payload;

    Frame response = decodeWhole(processRequest(std::move(frame), stopWork));

    require(response.version == VERSION);
    require(response.reserved == 0);
    require(response.requestId == requestId);

    if (response.type == TYPE_ERROR) {
        // Only COMPUTE can be refused here.
        require(type == TYPE_COMPUTE);
        require(response.payload.size() >= 2);
    }
    else {
        require(response.type == (type | RESPONSE_BIT));

        if (type == TYPE_ECHO)
            require(response.payload == payload);
        else
            require(response.payload.size() == 8);
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
    std::string_view input(reinterpret_cast<const char*>(data), size);
    DecodeResult decoded = decodeFrame(input, MAX_LENGTH);

    if (size < LENGTH_SIZE) {
        require(decoded.status == DecodeStatus::Incomplete);
        return 0;
    }

    std::uint32_t length = readU32(input.data());

    switch (decoded.status) {
    case DecodeStatus::Invalid:
        if (length < MIN_LENGTH)
            require(decoded.error == ErrorCode::MalformedFrame);
        else
            require(length > MAX_LENGTH &&
                    decoded.error == ErrorCode::PayloadTooLarge);
        break;

    case DecodeStatus::Incomplete:
        require(length >= MIN_LENGTH && length <= MAX_LENGTH);
        require(size - LENGTH_SIZE < length);
        require(decoded.consumed == 0);
        break;

    case DecodeStatus::Complete:
        require(length >= MIN_LENGTH && length <= MAX_LENGTH);
        require(decoded.consumed == LENGTH_SIZE + length);
        require(decoded.consumed <= size);
        require(decoded.frame.payload.size() == length - MIN_LENGTH);

        // Encoding what was decoded gives the same bytes back.
        require(encodeFrame(decoded.frame) ==
                input.substr(0, decoded.consumed));

        checkRequest(std::move(decoded.frame));
        break;
    }

    return 0;
}
