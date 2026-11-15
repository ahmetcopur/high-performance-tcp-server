// Fuzz target for a connection fed a byte stream in pieces.
//
// The first byte of the input says how to cut the rest up and whether
// the "worker" answers at once or a turn later. The rest is the byte
// stream a client sends. It goes through a real Connection on one end
// of a socket pair, the way the server drives one, without the event
// loop and the thread pool.
//
// What comes out is compared with what should: the same bytes decoded
// in one piece, by the plain rules of the protocol. Where the pieces
// are cut and when the answers come must make no difference to which
// responses the client gets, or to their order.
//
//   build-fuzz/fuzz_stream -max_total_time=60 tests/fuzz/corpus/stream

#include "Config.h"
#include "Connection.h"
#include "FileDescriptor.h"
#include "Protocol.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace protocol;

namespace {

// Small, so that the fuzzer can reach the limit.
constexpr std::uint32_t MAX_FRAME = 256;

void require(bool condition) {
    if (!condition)
        std::abort();
}

struct Answer {
    std::uint8_t type;
    std::uint32_t requestId;

    // The payload for a response, the error code for an ERROR.
    std::string payload;
    std::uint16_t code = 0;
};

bool sameAnswer(const Answer& a, const Answer& b) {
    return a.type == b.type && a.requestId == b.requestId &&
           a.payload == b.payload && a.code == b.code;
}

Answer errorAnswer(std::uint32_t requestId, ErrorCode code) {
    return {TYPE_ERROR, requestId, "", static_cast<std::uint16_t>(code)};
}

// What a client should get for this stream, whatever way it arrives.
std::vector<Answer> expectedAnswers(std::string_view stream) {
    std::vector<Answer> answers;

    while (true) {
        DecodeResult decoded = decodeFrame(stream, MAX_FRAME);

        if (decoded.status == DecodeStatus::Incomplete)
            break;

        // The stream is lost from here on.
        if (decoded.status == DecodeStatus::Invalid) {
            answers.push_back(errorAnswer(0, decoded.error));
            break;
        }

        stream.remove_prefix(decoded.consumed);

        ErrorCode error{};

        if (!validateRequest(decoded.frame, error)) {
            answers.push_back(errorAnswer(decoded.frame.requestId, error));
            continue;
        }

        answers.push_back(
            {static_cast<std::uint8_t>(decoded.frame.type | RESPONSE_BIT),
             decoded.frame.requestId, decoded.frame.payload});
    }

    return answers;
}

// What the client did get: every byte must belong to a whole frame.
std::vector<Answer> receivedAnswers(std::string_view bytes) {
    std::vector<Answer> answers;

    while (!bytes.empty()) {
        DecodeResult decoded = decodeFrame(bytes, 1 << 20);

        require(decoded.status == DecodeStatus::Complete);
        bytes.remove_prefix(decoded.consumed);

        Frame& frame = decoded.frame;
        require(frame.version == VERSION && frame.reserved == 0);

        if (frame.type == TYPE_ERROR) {
            require(frame.payload.size() >= 2);
            answers.push_back({TYPE_ERROR, frame.requestId, "",
                               readU16(frame.payload.data())});
        }
        else {
            answers.push_back({frame.type, frame.requestId, frame.payload});
        }
    }

    return answers;
}

// The same answer the server gives: the request's payload under the
// response type. What a worker computes is not what is tested here.
std::string answerTo(const Frame& request) {
    Frame response;
    response.type = static_cast<std::uint8_t>(request.type | RESPONSE_BIT);
    response.requestId = request.requestId;
    response.payload = request.payload;

    return encodeFrame(response);
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
    if (size < 1)
        return 0;

    // Low seven bits: the size of the pieces, 1 to 127 bytes, or the
    // whole stream at once for 0. Top bit: answers come a turn late.
    std::size_t piece = data[0] & 0x7F;
    bool late = (data[0] & 0x80) != 0;
    std::string_view stream(reinterpret_cast<const char*>(data) + 1, size - 1);

    if (piece == 0)
        piece = std::max<std::size_t>(stream.size(), 1);

    int fds[2];
    require(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    for (int fd : fds)
        require(fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) == 0);

    FileDescriptor peer(fds[1]);

    Config config;
    config.maxFrame = MAX_FRAME;
    Connection::Limits limits(config);
    Connection::Clock::time_point now = Connection::Clock::now();
    Connection connection(1, FileDescriptor(fds[0]), limits, now);

    std::string received;
    std::optional<std::string> owed;

    // One turn of the server for this connection.
    auto turn = [&]() {
        if (owed) {
            require(connection.finishWork(*owed));
            owed.reset();
        }

        if (connection.wantsRead())
            require(connection.receive());

        Frame request;

        while (connection.nextRequest(request)) {
            if (request.type == TYPE_PING) {
                connection.respond(answerTo(request));
                continue;
            }

            connection.startWork();

            if (late)
                owed = answerTo(request);
            else
                require(connection.finishWork(answerTo(request)));
        }

        require(connection.flush(now));
        connection.sendFinIfDue();

        char chunk[4096];
        ssize_t n;

        while ((n = read(peer.get(), chunk, sizeof(chunk))) > 0)
            received.append(chunk, static_cast<std::size_t>(n));
    };

    // More turns than any stream of this size can need. Running out of
    // them means the connection is stuck.
    std::size_t turnsLeft = 4 * stream.size() + 64;
    std::size_t sent = 0;

    while (sent < stream.size()) {
        std::size_t count = std::min(piece, stream.size() - sent);
        ssize_t n = write(peer.get(), stream.data() + sent, count);

        if (n > 0)
            sent += static_cast<std::size_t>(n);

        require(turnsLeft-- > 0);
        turn();
    }

    require(shutdown(peer.get(), SHUT_WR) == 0);

    while (!connection.finished()) {
        require(turnsLeft-- > 0);
        turn();
    }

    std::vector<Answer> expected = expectedAnswers(stream);
    std::vector<Answer> actual = receivedAnswers(received);

    require(actual.size() == expected.size());

    for (std::size_t i = 0; i < expected.size(); ++i)
        require(sameAnswer(actual[i], expected[i]));

    return 0;
}
