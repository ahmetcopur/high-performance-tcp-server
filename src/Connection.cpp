#include "Connection.h"

#include <algorithm>
#include <cerrno>
#include <utility>

#include <sys/socket.h>

namespace {

constexpr std::size_t READ_CHUNK = 64 * 1024;

} // namespace

const char* timeoutName(Phase phase) {
    switch (phase) {
    case Phase::Idle:
        return "idle connection";
    case Phase::Partial:
        return "frame not completed";
    case Phase::Writing:
        return "output not taken";
    case Phase::Lingering:
        return "peer did not close after an error";
    case Phase::Busy:
        break;
    }

    return "";
}

Connection::Limits::Limits(const Config& config)
    : maxFrame(static_cast<std::uint32_t>(config.maxFrame)),
      inputLimit(protocol::LENGTH_SIZE + maxFrame),
      outputHighWater(static_cast<std::size_t>(config.outputHighWater)),
      outputLowWater(outputHighWater / 2), idleTimeoutMs(config.idleTimeoutMs),
      partialFrameTimeoutMs(config.partialFrameTimeoutMs),
      writeStallTimeoutMs(config.writeStallTimeoutMs),
      lingerTimeoutMs(config.lingerTimeoutMs) {
}

int Connection::Limits::phaseLimitMs(Phase phase) const {
    switch (phase) {
    case Phase::Idle:
        return idleTimeoutMs;
    case Phase::Partial:
        return partialFrameTimeoutMs;
    case Phase::Writing:
        return writeStallTimeoutMs;
    case Phase::Lingering:
        return lingerTimeoutMs;
    case Phase::Busy:
        break;
    }

    // A request with a worker takes as long as it takes.
    return -1;
}

Connection::Connection(std::uint64_t id, FileDescriptor descriptor,
                       const Limits& sharedLimits, Clock::time_point now)
    : connectionId(id), socket(std::move(descriptor)), limits(&sharedLimits),
      phaseSince(now) {
}

bool Connection::receive() {
    char chunk[READ_CHUNK];

    // In Closing the bytes are dropped, so the limit only bounds how
    // much is taken in one go.
    std::size_t room = limits->inputLimit;

    if (currentState == State::Open)
        room -= input.size();

    while (room > 0) {
        std::size_t wanted = std::min(sizeof(chunk), room);
        ssize_t n = recv(socket.get(), chunk, wanted, 0);

        if (n > 0) {
            auto count = static_cast<std::size_t>(n);

            if (currentState == State::Open)
                input.append({chunk, count});

            room -= count;

            // Less than was asked for, so the socket is empty now, and
            // asking again would only be told so. If more arrives, or
            // the peer closes, the poller reports the socket again. That
            // holds because the pollers are level-triggered: they report
            // a socket for as long as it has something, not once.
            if (count < wanted)
                break;
        }
        else if (n == 0) {
            currentState = State::ReadClosed;
            break;
        }
        else if (errno == EINTR) {
            continue;
        }
        else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }
        else {
            return false;
        }
    }

    return true;
}

bool Connection::flush(Clock::time_point now) {
    while (output.size() > 0) {
        ssize_t n = send(socket.get(), output.data(), output.size(), 0);

        if (n > 0) {
            output.consume(static_cast<std::size_t>(n));

            // Output moved, so the peer is not stalled.
            phaseSince = now;
        }
        else if (n < 0 && errno == EINTR) {
            continue;
        }
        else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        }
        else {
            return false;
        }
    }

    if (output.size() <= limits->outputLowWater)
        throttled = false;

    return true;
}

bool Connection::nextRequest(protocol::Frame& request) {
    using namespace protocol;

    while (currentState != State::Closing && !working && !throttled) {
        DecodeResult decoded = decodeFrame(input.view(), limits->maxFrame);

        if (decoded.status == DecodeStatus::Incomplete) {
            // Once reading is over, a partial frame can never be
            // completed.
            if (currentState == State::ReadClosed)
                input.clear();

            return false;
        }

        if (decoded.status == DecodeStatus::Invalid) {
            // Without a usable length there is no telling where the next
            // frame starts. Answer and serve nothing more. If the peer
            // has already finished sending, the connection closes once
            // the answer is out.
            respond(encodeError(0, decoded.error, errorName(decoded.error)));

            input.clear();

            if (currentState == State::Open)
                currentState = State::Closing;

            return false;
        }

        input.consume(decoded.consumed);

        ErrorCode error;

        if (!validateRequest(decoded.frame, error)) {
            respond(
                encodeError(decoded.frame.requestId, error, errorName(error)));
            continue;
        }

        request = std::move(decoded.frame);
        return true;
    }

    return false;
}

void Connection::respond(std::string_view encoded) {
    output.append(encoded);

    if (output.size() >= limits->outputHighWater)
        throttled = true;
}

void Connection::startWork() {
    working = true;
}

bool Connection::finishWork(std::string_view encoded) {
    working = false;

    // Behind output that is still waiting, the response has to wait
    // too. Otherwise it is offered to the socket first. Copying a large
    // response into the output only to send it from there a moment
    // later was a fifth of the event loop's time with 64 KiB payloads.
    while (output.size() == 0 && !encoded.empty()) {
        ssize_t n = send(socket.get(), encoded.data(), encoded.size(), 0);

        if (n > 0)
            encoded.remove_prefix(static_cast<std::size_t>(n));
        else if (n < 0 && errno == EINTR)
            continue;
        else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            break;
        else
            return false;
    }

    respond(encoded);
    return true;
}

void Connection::drain() {
    input.clear();

    if (currentState == State::Open)
        currentState = State::Closing;
}

void Connection::sendFinIfDue() {
    if (currentState != State::Closing || working || output.size() > 0 ||
        finSent)
        return;

    shutdown(socket.get(), SHUT_WR);
    finSent = true;
}

bool Connection::owesNothing() const {
    return !working && output.size() == 0;
}

bool Connection::finished() const {
    return currentState == State::ReadClosed && !working && input.size() == 0 &&
           output.size() == 0;
}

bool Connection::wantsRead() const {
    if (currentState == State::ReadClosed)
        return false;

    if (currentState == State::Closing)
        return true;

    return !throttled && input.size() < limits->inputLimit;
}

bool Connection::wantsWrite() const {
    return output.size() > 0;
}

Phase Connection::computePhase() const {
    if (output.size() > 0)
        return Phase::Writing;

    if (working)
        return Phase::Busy;

    if (currentState == State::Closing)
        return Phase::Lingering;

    // Complete frames never wait here while nothing is in flight, so
    // whatever is left is the start of a frame.
    if (input.size() > 0)
        return Phase::Partial;

    return Phase::Idle;
}

long long Connection::timeLeftMs(Clock::time_point now) {
    Phase phase = computePhase();

    if (phase != currentPhase) {
        currentPhase = phase;
        phaseSince = now;
    }

    int limitMs = limits->phaseLimitMs(phase);

    if (limitMs < 0)
        return -1;

    auto deadline = phaseSince + std::chrono::milliseconds(limitMs);

    long long remainingMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
            .count();

    return std::max(remainingMs, 0LL);
}
