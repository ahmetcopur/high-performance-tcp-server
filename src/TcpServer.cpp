#include "TcpServer.h"

#include "Protocol.h"

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

// Runs inside a worker thread. Handles ECHO and COMPUTE, the only types
// the event loop hands over, and returns the encoded response.
std::string processRequest(protocol::Frame request,
                           const std::atomic<bool>& stopWork) {
    using namespace protocol;

    Frame response;
    response.type = static_cast<std::uint8_t>(request.type | RESPONSE_BIT);
    response.requestId = request.requestId;

    if (request.type == TYPE_ECHO) {
        response.payload = std::move(request.payload);
        return encodeFrame(response);
    }

    if (request.payload.size() < 4)
        return encodeError(request.requestId, ErrorCode::BadRequest,
                           "compute payload shorter than 4 bytes");

    std::uint32_t iterations = readU32(request.payload.data());

    if (iterations > MAX_COMPUTE_ITERATIONS)
        return encodeError(request.requestId, ErrorCode::BadRequest,
                           "iteration count above the limit");

    std::string_view data(request.payload.data() + 4,
                          request.payload.size() - 4);

    appendU64(response.payload, computeHash(data, iterations, &stopWork));
    return encodeFrame(response);
}

[[noreturn]] void fail(const char* what) {
    throw std::system_error(errno, std::generic_category(), what);
}

} // namespace

TcpServer::TcpServer(const Config& serverConfig)
    : config(serverConfig), limits(config), loop(config.poller),
      pool(static_cast<std::size_t>(config.workers)) {
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

    if (completion.failed) {
        closeConnection(it);
        return;
    }

    it->second.finishWork(completion.response);
    advance(it->second, now);
}

int TcpServer::onTimers(Clock::time_point now) {
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

        connection.startWork();

        try {
            pool.enqueue([this, fd, id = connection.id(),
                          request = std::move(request)]() mutable {
                Completion completion{fd, id, "", false};

                // An exception must still produce a completion, or the
                // connection would wait for its worker forever.
                try {
                    completion.response =
                        processRequest(std::move(request), stopWork);
                } catch (...) {
                    completion.failed = true;
                }

                loop.post(std::move(completion));
            });
        } catch (const std::exception& e) {
            std::cerr << "Task error: " << e.what() << '\n';
            connection.stopServing();
        }
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
