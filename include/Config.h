#pragma once

#include <string>

#include <sys/socket.h>

// The system call the event loop waits with.
enum class PollerKind {
    Poll,
    Epoll,
};

struct Config {
    // epoll exists only on Linux, and is the default there.
#ifdef __linux__
    PollerKind poller = PollerKind::Epoll;
#else
    PollerKind poller = PollerKind::Poll;
#endif

    int port = 8080;
    int workers = 8;
    int maxConnections = 10000;
    int backlog = SOMAXCONN;

    // Largest value a frame's length field may have.
    int maxFrame = 1 << 20;

    // Pending output per connection above which the server stops reading
    // from it. Reading resumes at half of this.
    int outputHighWater = 4 << 20;

    // How long a connection may stay in each waiting state, in
    // milliseconds, before the server closes it.

    // Nothing buffered and no request in flight.
    int idleTimeoutMs = 60000;

    // A frame has started to arrive and is not complete.
    int partialFrameTimeoutMs = 10000;

    // Output is waiting and the peer takes none of it.
    int writeStallTimeoutMs = 10000;

    // After a framing error: our side is closed, the peer's is not yet.
    int lingerTimeoutMs = 2000;

    // After SIGINT or SIGTERM: how long connections get to receive the
    // responses they are owed before everything is closed.
    int shutdownTimeoutMs = 5000;
};

// Reads "--name value" pairs into config. On failure returns false and
// puts a message for the user in error.
bool parseConfig(int argc, const char* const* argv, Config& config,
                 std::string& error);

// One line per option, for the usage message.
std::string optionSummary();

// "poll" or "epoll".
const char* pollerName(PollerKind kind);
