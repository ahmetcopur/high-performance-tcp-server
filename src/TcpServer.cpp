#include "TcpServer.h"

#include "Protocol.h"
#include "RequestHandler.h"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>

#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>

namespace {

constexpr int ACCEPT_RETRY_MS = 100;

[[noreturn]] void fail(const char* what) {
    throw std::system_error(errno, std::generic_category(), what);
}

} // namespace

TcpServer::TcpServer(const Config& serverConfig)
    : config(serverConfig), limits(config), loop(config.poller),
      pool(static_cast<std::size_t>(config.workers),
           static_cast<std::size_t>(config.queueCapacity)),
      startedAt(Clock::now()), nextStatsLog(startedAt) {
    listener.reset(socket(AF_INET, SOCK_STREAM, 0));

    if (!listener.valid())
        fail("socket");

    int reuse = 1;

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(static_cast<std::uint16_t>(config.port));

    if (setsockopt(listener.get(), SOL_SOCKET, SO_REUSEADDR, &reuse,
                   sizeof(reuse)) < 0)
        fail("setsockopt");

    if (!listener.setNonBlocking())
        fail("fcntl");

    if (bind(listener.get(), reinterpret_cast<sockaddr*>(&address),
             sizeof(address)) < 0)
        fail("bind");

    if (listen(listener.get(), config.backlog) < 0)
        fail("listen");

    spare.reset(open("/dev/null", O_RDONLY));

    if (!spare.valid())
        fail("open /dev/null");

    if (!loop.poller().add(listener.get(), true, false))
        fail("watching the listening socket");
}

// Workers post to the loop, so they must be gone before it is. run()
// already stops them. This covers a server that was never run, and makes
// the order hold whatever order the members are declared in.
TcpServer::~TcpServer() {
    pool.shutdown();
}

void TcpServer::requestShutdown() {
    loop.requestShutdown();
}

void TcpServer::run() {
    loop.run(*this);

    // The end of a shutdown, in the order it must happen.

    // Connections first. Any that are left here ran out of time.
    connections.clear();
    listener.reset();
    spare.reset();

    // Then the workers. Whatever they are still computing belongs to a
    // connection that no longer exists, so they are told to give up.
    // shutdown() returns when every worker thread has ended. From then on
    // this is the only thread, and nothing else can reach the loop's
    // completion queue or its wakeup pipe.
    stopWork = true;
    pool.shutdown();
}

void TcpServer::logLimited(Clock::time_point& last, const char* source,
                           const char* message) {
    auto now = Clock::now();

    if (now - last < std::chrono::seconds(1))
        return;

    last = now;
    std::cerr << source << ": " << message << '\n';
}

void TcpServer::closeConnection(int fd) {
    auto it = connections.find(fd);

    if (it != connections.end())
        closeConnection(it);
}

TcpServer::ConnectionMap::iterator
TcpServer::closeConnection(ConnectionMap::iterator it) {
    timers.cancel(it->second.id());
    loop.poller().remove(it->first);

    return connections.erase(it);
}

void TcpServer::settle(Connection& connection, Clock::time_point now) {
    loop.poller().modify(connection.fd(), connection.wantsRead(),
                         connection.wantsWrite());

    long long leftMs = connection.timeLeftMs(now);

    // A phase without a limit is not armed. An earlier entry may still
    // come up, and is then found to have nothing to do.
    if (leftMs >= 0)
        timers.arm(connection.fd(), connection.id(),
                   now + std::chrono::milliseconds(leftMs));
}

void TcpServer::onShutdownRequested() {
    Clock::time_point now = Clock::now();

    draining = true;
    drainDeadline = now + std::chrono::milliseconds(config.shutdownTimeoutMs);

    // No new connections from here on.
    loop.poller().remove(listener.get());
    listener.reset();

    for (auto it = connections.begin(); it != connections.end();) {
        Connection& connection = it->second;

        // Requests that have not been started are not started. What is
        // owed is still sent, and the connection then ends as it does
        // after a framing error: FIN first, and close once the peer has
        // closed too.
        connection.drain();

        if (connection.owesNothing()) {
            it = closeConnection(it);
            continue;
        }

        settle(connection, now);
        ++it;
    }
}

void TcpServer::onCompletion(Completion& completion, Clock::time_point now) {
    auto it = connections.find(completion.fd);

    // The connection is gone, or the descriptor now belongs to another.
    if (it == connections.end() || it->second.id() != completion.id)
        return;

    // The worker could not produce even an error. There is nothing to
    // send, and the client must not be left waiting.
    if (completion.response.empty()) {
        closeConnection(it);
        return;
    }

    it->second.finishWork(completion.response);
    advance(it->second, now);
}

int TcpServer::onTimers(Clock::time_point now) {
    // The loop is about to wait. Requests taken in this turn were queued
    // without waking anyone, because waking a thread costs more than a
    // small request does, and a turn can bring dozens of them. Now each
    // sleeping worker that is needed is woken once. Nothing is lost by
    // the delay: an answer cannot be sent before the next turn anyway.
    pool.wakeWorkers();

    if (draining && connections.empty()) {
        loop.stop();
        return -1;
    }

    // Connections whose time has come up. An entry only promises that
    // the connection is due no later than this, so each one is asked
    // again. If it is not due after all, it goes back in.
    TimerQueue::Entry entry;

    while (timers.popDue(now, entry)) {
        auto it = connections.find(entry.fd);

        if (it == connections.end() || it->second.id() != entry.id)
            continue;

        Connection& connection = it->second;
        long long leftMs = connection.timeLeftMs(now);

        if (leftMs == 0) {
            logLimited(lastTimeoutLog, "timeout",
                       timeoutName(connection.phase()));
            closeConnection(it);
        }
        else if (leftMs > 0) {
            timers.arm(entry.fd, entry.id,
                       now + std::chrono::milliseconds(leftMs));
        }
    }

    long long timeoutMs = timers.msUntilNext(now);

    if (config.statsIntervalMs > 0) {
        auto interval = std::chrono::milliseconds(config.statsIntervalMs);

        if (now >= nextStatsLog) {
            logStats(now);
            nextStatsLog = now + interval;
        }

        long long untilNextMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(nextStatsLog -
                                                                  now)
                .count() +
            1;

        if (timeoutMs < 0 || untilNextMs < timeoutMs)
            timeoutMs = untilNextMs;
    }

    if (acceptBackoff && (timeoutMs < 0 || ACCEPT_RETRY_MS < timeoutMs))
        timeoutMs = ACCEPT_RETRY_MS;

    if (draining) {
        long long remainingMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                drainDeadline - now)
                .count();

        if (remainingMs <= 0) {
            std::cerr << "shutdown: time is up, closing " << connections.size()
                      << " connection(s)\n";
            loop.stop();
            return -1;
        }

        // One more than the truncated remainder, so that the loop wakes
        // after the deadline and not just before it.
        if (timeoutMs < 0 || remainingMs + 1 < timeoutMs)
            timeoutMs = remainingMs + 1;
    }
    else {
        // At the connection limit the listening socket is not watched, so
        // new connections wait in the kernel backlog and the loop does
        // not keep waking for a socket it is not going to accept from.
        bool accepting = connections.size() <
                             static_cast<std::size_t>(config.maxConnections) &&
                         !acceptBackoff;

        loop.poller().modify(listener.get(), accepting, false);
    }

    acceptBackoff = false;

    return static_cast<int>(timeoutMs);
}

void TcpServer::onSocketEvent(const PollEvent& event, Clock::time_point now) {
    if (event.fd == listener.get()) {
        if (event.readable)
            acceptConnections(now);

        return;
    }

    auto it = connections.find(event.fd);

    if (it == connections.end())
        return;

    Connection& connection = it->second;

    if (event.error) {
        int error = 0;
        socklen_t length = sizeof(error);

        if (getsockopt(event.fd, SOL_SOCKET, SO_ERROR, &error, &length) == 0 &&
            error != 0)
            logLimited(lastConnectionLog, "connection", std::strerror(error));

        closeConnection(event.fd);
        return;
    }

    bool reading = connection.wantsRead();

    // A hangup says the peer will send no more. While we are still
    // reading, receive() below collects what is left and then finds the
    // end, and responses we owe can still go out. When we are not
    // reading, there is nothing left to learn from this socket and nobody
    // to write to.
    if (event.hangup && !reading) {
        closeConnection(event.fd);
        return;
    }

    bool ok = true;

    if (event.writable)
        ok = connection.flush(now);

    if (ok && reading && (event.readable || event.hangup))
        ok = connection.receive();

    if (!ok) {
        closeConnection(event.fd);
        return;
    }

    advance(connection, now);
}

void TcpServer::acceptConnections(Clock::time_point now) {
    while (connections.size() <
           static_cast<std::size_t>(config.maxConnections)) {
        FileDescriptor socket(accept(listener.get(), nullptr, nullptr));

        // Out of descriptors. On Linux the connection stays in the
        // backlog and keeps the listening socket readable, so the loop
        // would wake at once, forever. Give up the spare descriptor,
        // accept the connection into its slot, and close it again.
        if (!socket.valid() && (errno == EMFILE || errno == ENFILE)) {
            logLimited(lastAcceptLog, "accept",
                       "out of file descriptors, refusing new connections");

            spare.reset();

            socket.reset(accept(listener.get(), nullptr, nullptr));
            int acceptErrno = errno;
            bool refused = socket.valid();

            socket.reset();
            spare.reset(open("/dev/null", O_RDONLY));

            if (refused)
                continue;

            errno = acceptErrno;
        }

        if (!socket.valid()) {
            if (errno == EINTR || errno == ECONNABORTED)
                continue;

            // Anything else that is not "queue empty" may leave a
            // connection in the backlog, so back off instead of watching
            // the listening socket again right away.
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                logLimited(lastAcceptLog, "accept", std::strerror(errno));
                acceptBackoff = true;
            }

            break;
        }

        if (!socket.setNonBlocking())
            continue;

        int fd = socket.get();

        // The connection is dropped again if it cannot be watched.
        if (!loop.poller().add(fd, true, false)) {
            logLimited(lastAcceptLog, "accept", std::strerror(errno));
            continue;
        }

        auto added = connections.emplace(
            std::piecewise_construct, std::forward_as_tuple(fd),
            std::forward_as_tuple(nextId++, std::move(socket), limits, now));

        settle(added.first->second, now);
    }
}

void TcpServer::advance(Connection& connection, Clock::time_point now) {
    int fd = connection.fd();

    while (true) {
        dispatch(connection, now);

        if (!connection.wantsWrite())
            break;

        // Send at once. The socket nearly always has room, and asking the
        // poller first costs two epoll_ctl calls and a turn of the loop
        // for every response. The poller is asked only when the socket
        // does not take everything.
        bool wasHeldBack = connection.heldBack();

        if (!connection.flush(now)) {
            closeConnection(fd);
            return;
        }

        // Sending made room, so requests that were held back by the full
        // output can be decoded now. Nothing else would bring them up
        // again: the client may have sent all it is going to send.
        if (!wasHeldBack || connection.heldBack())
            break;
    }

    // In Closing, once the last response has gone out: send FIN and keep
    // the socket open for reading, so that bytes the peer is still
    // sending do not turn the close into a reset that could destroy what
    // was just sent.
    connection.sendFinIfDue();

    if (connection.finished()) {
        closeConnection(fd);
        return;
    }

    settle(connection, now);
}

void TcpServer::dispatch(Connection& connection, Clock::time_point now) {
    using namespace protocol;

    int fd = connection.fd();
    Frame request;

    while (connection.nextRequest(request)) {
        // Cheap enough to answer on this thread. It still waited its turn
        // behind any request that was with a worker.
        if (request.type == TYPE_PING) {
            Frame response;
            response.type = TYPE_PING | RESPONSE_BIT;
            response.requestId = request.requestId;

            connection.respond(encodeFrame(response));
            continue;
        }

        // Answered here too. It reports on the workers, so it must not
        // have to wait for one.
        if (request.type == TYPE_STATS) {
            Frame response;
            response.type = TYPE_STATS | RESPONSE_BIT;
            response.requestId = request.requestId;
            response.payload = encodeStats(currentStats(now));

            connection.respond(encodeFrame(response));
            continue;
        }

        std::uint32_t requestId = request.requestId;

        // Queued without waking a worker. That is done once for the
        // whole turn of the loop, in onTimers().
        bool accepted =
            pool.tryQueue([this, fd, id = connection.id(),
                            request = std::move(request)]() mutable {
                Completion completion{fd, id, ""};
                std::uint32_t failedId = request.requestId;

                // Something goes back to the loop whatever happens here, or
                // the connection would wait for its worker forever. If even
                // the error cannot be built, the response stays empty.
                try {
                    completion.response =
                        answerOrInternalError(failedId, [&]() {
                            return processRequest(std::move(request), stopWork);
                        });
                } catch (...) {
                }

                loop.post(std::move(completion));
            });

        if (accepted) {
            connection.startWork();
            continue;
        }

        // The queue is full. Say so and carry on: the connection stays
        // open, and its next request may find room.
        connection.respond(encodeError(requestId, ErrorCode::ServerBusy,
                                       errorName(ErrorCode::ServerBusy)));
    }
}

protocol::ServerStats TcpServer::currentStats(Clock::time_point now) const {
    ThreadPool::Stats work = pool.stats();
    protocol::ServerStats stats;

    stats.connections = connections.size();
    stats.queueDepth = work.queueDepth;
    stats.queueCapacity = work.capacity;
    stats.workers = work.workers;
    stats.tasksSubmitted = work.submitted;
    stats.tasksRejected = work.rejected;
    stats.tasksCompleted = work.completed;
    stats.tasksFailed = work.failed;
    stats.workerBusyMicroseconds = work.busyMicroseconds;
    stats.uptimeMicroseconds = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(now - startedAt)
            .count());

    return stats;
}

void TcpServer::logStats(Clock::time_point now) const {
    protocol::ServerStats stats = currentStats(now);

    std::cerr << "stats: connections=" << stats.connections
              << " queue=" << stats.queueDepth << '/' << stats.queueCapacity
              << " submitted=" << stats.tasksSubmitted
              << " rejected=" << stats.tasksRejected
              << " completed=" << stats.tasksCompleted
              << " failed=" << stats.tasksFailed
              << " busy_ms=" << stats.workerBusyMicroseconds / 1000 << '\n';
}
