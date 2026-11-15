#pragma once

#include "Buffer.h"
#include "Config.h"
#include "FileDescriptor.h"
#include "Protocol.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>

// Where a connection is in its life. docs/architecture.md has the
// transitions and the rules that hold in each state.
enum class State {
    // Requests are read and answered.
    Open,

    // The peer has finished sending. Nothing more is read. Requests
    // already received are still answered, then the connection closes.
    ReadClosed,

    // The conversation is over: a framing error, or a shutdown. What is
    // owed is sent, then our side of the connection is shut down. Input
    // is read only to be thrown away, until the peer closes too.
    Closing,
};

// What a connection is waiting for right now. Every phase except Busy
// has a time limit.
enum class Phase {
    Idle,      // nothing buffered and no request in flight
    Partial,   // a frame has started to arrive and is not complete
    Busy,      // a worker has the request
    Writing,   // output is waiting for the peer to take it
    Lingering, // our side is shut down, waiting for the peer to close
};

// For the log line written when a phase runs out of time.
const char* timeoutName(Phase phase);

// One client connection: its socket, its buffers, its state and its
// deadlines. Only the event loop thread touches a Connection. Workers
// never see one.
class Connection {
public:
    using Clock = std::chrono::steady_clock;

    // The limits every connection of a server shares.
    struct Limits {
        // Largest value a frame's length field may have.
        std::uint32_t maxFrame;

        // Room for exactly one frame of the largest size. A valid frame
        // always fits, so a full input buffer always holds a complete one.
        std::size_t inputLimit;

        // Above the high mark reading and decoding stop. At or below the
        // low mark they resume.
        std::size_t outputHighWater;
        std::size_t outputLowWater;

        int idleTimeoutMs;
        int partialFrameTimeoutMs;
        int writeStallTimeoutMs;
        int lingerTimeoutMs;

        explicit Limits(const Config& config);

        // Time limit of a phase in milliseconds, or -1 if it has none.
        int phaseLimitMs(Phase phase) const;
    };

private:
    std::uint64_t connectionId;
    FileDescriptor socket;
    const Limits* limits;

    State currentState = State::Open;

    // Bytes received and not yet decoded. Requests wait here as raw
    // bytes until the request before them has been answered.
    Buffer input;

    // Encoded responses not yet sent.
    Buffer output;

    // One request is with the worker pool. Nothing more is decoded until
    // its response is queued, which keeps responses in request order.
    bool working = false;

    // Output passed the high watermark. Reading and decoding stop until
    // it has drained to the low watermark.
    bool throttled = false;

    // In Closing: the output is sent and shutdown(SHUT_WR) was called.
    bool finSent = false;

    Phase currentPhase = Phase::Idle;

    // When the current phase began, or when output last made progress.
    Clock::time_point phaseSince;

    Phase computePhase() const;

public:
    // limits must outlive the connection.
    Connection(std::uint64_t id, FileDescriptor socket, const Limits& limits,
               Clock::time_point now);

    std::uint64_t id() const {
        return connectionId;
    }

    int fd() const {
        return socket.get();
    }

    State state() const {
        return currentState;
    }

    // The phase found by the last call to timeLeftMs().
    Phase phase() const {
        return currentPhase;
    }

    // Takes what the socket has, up to the input limit. In Closing the
    // bytes are dropped. Returns false if the socket failed.
    bool receive();

    // Sends as much pending output as the socket takes. Returns false if
    // the socket failed.
    bool flush(Clock::time_point now);

    // Decodes requests from the input until one is valid and can be
    // served, and returns it. Requests that are not valid are answered
    // with an ERROR here. Returns false when nothing can be served now:
    // no complete frame, a request with a worker, output above the high
    // watermark, or a connection that no longer serves.
    bool nextRequest(protocol::Frame& request);

    // Queues an encoded response for sending.
    void respond(std::string_view encoded);

    // The request just returned by nextRequest() goes to a worker.
    void startWork();

    // The worker is done. Queues its encoded response.
    void finishWork(std::string_view encoded);

    // A shutdown has begun. Requests not yet started are dropped, and a
    // connection that was Open ends through Closing.
    void drain();

    // In Closing, once nothing is left to send: shuts down our side, so
    // the peer sees FIN while its own bytes can still be read and dropped.
    void sendFinIfDue();

    // No request with a worker and nothing waiting to be sent.
    bool owesNothing() const;

    // The peer has closed and there is nothing left to decode, compute or
    // send. Nothing is unread either, so closing sends FIN, not a reset.
    bool finished() const;

    // Whether to watch the socket for input. The same answer decides
    // whether a reported hangup still has input behind it.
    bool wantsRead() const;

    bool wantsWrite() const;

    // Output passed the high watermark and has not drained to the low
    // one yet. Nothing is decoded while this holds.
    bool heldBack() const {
        return throttled;
    }

    // Works out the current phase and how long it may still last.
    // Returns the milliseconds left, 0 if the time is up, or -1 if the
    // phase has no limit.
    long long timeLeftMs(Clock::time_point now);
};
