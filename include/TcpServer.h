#pragma once

#include "Config.h"
#include "Connection.h"
#include "EventLoop.h"
#include "FileDescriptor.h"
#include "ThreadPool.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <unordered_map>

// The server: it accepts connections, keeps them in a map, hands their
// requests to the worker pool, and decides how a shutdown goes.
//
// Everything in here except requestShutdown() and the bodies of the
// tasks given to the pool runs on the thread that called run(). Workers
// get a copy of the request and the connection's id. They reach back
// only through EventLoop::post().
class TcpServer : private EventLoop::Handler {
private:
    using Clock = EventLoop::Clock;

    Config config;
    Connection::Limits limits;

    EventLoop loop;

    FileDescriptor listener;

    // Held in reserve. When the process runs out of descriptors, closing
    // this one makes room to accept a pending connection and close it.
    FileDescriptor spare;

    ThreadPool pool;

    // Set at the very end of a shutdown. Requests still running then
    // belong to connections that no longer exist, and COMPUTE gives up
    // when it sees this.
    std::atomic<bool> stopWork{false};

    std::unordered_map<int, Connection> connections;

    // Never reused, unlike descriptor numbers.
    std::uint64_t nextId = 1;

    // Set when accept() fails for a reason that retrying at once cannot
    // fix. The listening socket then sits out one round.
    bool acceptBackoff = false;

    bool draining = false;
    Clock::time_point drainDeadline;

    Clock::time_point lastAcceptLog;
    Clock::time_point lastConnectionLog;
    Clock::time_point lastTimeoutLog;

    void onShutdownRequested() override;
    void onCompletion(Completion& completion) override;
    int onTimers(Clock::time_point now) override;
    void onSocketEvent(const PollEvent& event, Clock::time_point now) override;

    void acceptConnections(Clock::time_point now);

    // Serves what the connection has received, as far as it can go now,
    // then closes the connection if that was the last thing it needed.
    void advance(Connection& connection);

    void closeConnection(int fd);

    // A flood of failures must not become a flood of log lines. Each
    // source writes at most one line per second.
    void logLimited(Clock::time_point& last, const char* source,
                    const char* message);

public:
    // Opens the listening socket and starts the workers. Throws
    // std::system_error if the server cannot be set up.
    explicit TcpServer(const Config& config);

    ~TcpServer();

    // Serves until a shutdown has finished.
    void run();

    // The first call starts an orderly shutdown. Safe from a signal
    // handler.
    void requestShutdown();
};
