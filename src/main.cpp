
#include "Buffer.h"
#include "Config.h"
#include "Protocol.h"
#include "ThreadPool.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <mutex>
#include <poll.h>
#include <queue>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

constexpr std::size_t READ_CHUNK = 64 * 1024;
constexpr int ACCEPT_RETRY_MS = 100;

volatile std::sig_atomic_t running = 1;

// Write end of the wake pipe. The event loop polls the read end.
int wakeWriteFd = -1;

// Makes a sleeping poll() return. Safe to call from worker threads
// and from a signal handler. If the pipe is full the write fails with
// EAGAIN, which is fine because a wakeup is already pending.
void wake() {
    int saved = errno;
    char byte = 1;
    ssize_t n = write(wakeWriteFd, &byte, 1);
    (void)n;
    errno = saved;
}

// Where a connection is in its life. docs/architecture.md has the
// transitions and the rules that hold in each state.
enum class State {
    // Requests are read and answered.
    Open,

    // The peer has finished sending. Nothing more is read. Requests
    // already received are still answered, then the connection closes.
    ReadClosed,

    // A framing error ended the conversation. The error is sent, then
    // our side of the connection is shut down. Input is read only to be
    // thrown away, until the peer closes too.
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

struct Client {
    std::uint64_t id = 0;

    State state = State::Open;

    // Bytes received and not yet decoded. Requests wait here as raw
    // bytes until the request before them has been answered.
    Buffer input;

    // Encoded responses not yet sent.
    Buffer output;

    // One request is with the worker pool. Nothing more is decoded until
    // its response is queued, which keeps responses in request order.
    bool processing = false;

    // Output passed the high watermark. Reading and decoding stop until
    // it has drained to the low watermark.
    bool throttled = false;

    // In Closing: the output is sent and shutdown(SHUT_WR) was called.
    bool finSent = false;

    Phase phase = Phase::Idle;

    // When the current phase began, or when output last made progress.
    std::chrono::steady_clock::time_point phaseSince;
};

Phase phaseOf(const Client& client) {
    if (client.output.size() > 0)
        return Phase::Writing;

    if (client.state == State::Closing)
        return Phase::Lingering;

    if (client.processing)
        return Phase::Busy;

    // Complete frames never wait here while nothing is in flight, so
    // whatever is left is the start of a frame.
    if (client.input.size() > 0)
        return Phase::Partial;

    return Phase::Idle;
}

// For the log line written when a phase runs out of time.
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

struct Completion {
    int fd;
    std::uint64_t id;
    std::string response;
    bool failed;
};

// Make a socket non-blocking
bool setNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);

    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) >= 0;
}

// Runs inside a worker thread. Handles ECHO and COMPUTE, the only types
// the event loop hands over, and returns the encoded response.
std::string processRequest(protocol::Frame request) {
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

    appendU64(response.payload, computeHash(data, iterations));
    return encodeFrame(response);
}

int main(int argc, char** argv) {
    Config config;
    std::string configError;

    if (!parseConfig(argc, argv, config, configError)) {
        std::cerr << configError << "\n"
                  << "Usage: " << argv[0] << " [option value]...\n"
                  << optionSummary();
        return 1;
    }

    int wakePipe[2];

    if (pipe(wakePipe) < 0 || !setNonBlocking(wakePipe[0]) ||
        !setNonBlocking(wakePipe[1])) {
        perror("pipe");
        return 1;
    }

    wakeWriteFd = wakePipe[1];

    auto onSignal = [](int) {
        running = 0;
        wake();
    };

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    std::signal(SIGPIPE, SIG_IGN);

    // 1. Create server socket
    int serverSocket = socket(AF_INET, SOCK_STREAM, 0);

    if (serverSocket < 0) {
        perror("socket");
        return 1;
    }

    int opt = 1;

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(static_cast<std::uint16_t>(config.port));

    // 2. Configure server
    if (setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) <
            0 ||
        !setNonBlocking(serverSocket) ||
        bind(serverSocket, reinterpret_cast<sockaddr*>(&address),
             sizeof(address)) < 0 ||
        listen(serverSocket, config.backlog) < 0) {

        perror("server setup");
        close(serverSocket);
        return 1;
    }

    // Held in reserve. When the process runs out of descriptors, closing
    // this one makes room to accept a pending connection and close it.
    int spareFd = open("/dev/null", O_RDONLY);

    if (spareFd < 0) {
        perror("open /dev/null");
        close(serverSocket);
        return 1;
    }

    // 3. Create Thread Pool and shared result queue
    std::mutex completionMutex;
    std::queue<Completion> completions;

    ThreadPool pool(static_cast<std::size_t>(config.workers));

    std::unordered_map<int, Client> clients;
    std::uint64_t nextId = 1;

    const auto maxConnections = static_cast<std::size_t>(config.maxConnections);
    const auto maxFrame = static_cast<std::uint32_t>(config.maxFrame);

    // Room for exactly one frame of the largest size. A valid frame
    // always fits, so a full input buffer always holds a complete one.
    const std::size_t inputLimit = protocol::LENGTH_SIZE + maxFrame;

    const auto outputHighWater =
        static_cast<std::size_t>(config.outputHighWater);
    const std::size_t outputLowWater = outputHighWater / 2;

    // Time limit of each phase in milliseconds, indexed by Phase. A
    // request with a worker takes as long as it takes.
    const int phaseLimitMs[] = {
        config.idleTimeoutMs,         // Idle
        config.partialFrameTimeoutMs, // Partial
        -1,                           // Busy
        config.writeStallTimeoutMs,   // Writing
        config.lingerTimeoutMs,       // Lingering
    };

    auto closeClient = [&](int fd) {
        close(fd);
        clients.erase(fd);
    };

    // Set when accept() fails for a reason that retrying at once cannot
    // fix. The listening socket then sits out one poll() round.
    bool acceptBackoff = false;

    std::chrono::steady_clock::time_point lastAcceptLog;
    std::chrono::steady_clock::time_point lastConnectionLog;
    std::chrono::steady_clock::time_point lastTimeoutLog;

    // A flood of failures must not become a flood of log lines. Each
    // source writes at most one line per second.
    auto logLimited = [](std::chrono::steady_clock::time_point& last,
                         const char* source, const char* message) {
        auto now = std::chrono::steady_clock::now();

        if (now - last < std::chrono::seconds(1))
            return;

        last = now;
        std::cerr << source << ": " << message << '\n';
    };

    // The same test decides whether to ask poll() for POLLIN and whether
    // a reported hangup still has input behind it.
    auto wantsRead = [&](const Client& client) {
        if (client.state == State::ReadClosed)
            return false;

        if (client.state == State::Closing)
            return true;

        return !client.throttled && client.input.size() < inputLimit;
    };

    auto queueResponse = [&](Client& client, std::string_view response) {
        client.output.append(response);

        if (client.output.size() >= outputHighWater)
            client.throttled = true;
    };

    // Decodes requests from the front of the input buffer and acts on
    // them, until one goes to a worker, the output buffer is full, or
    // the input holds no complete frame.
    auto serveRequests = [&](int fd, Client& client) {
        using namespace protocol;

        while (client.state != State::Closing && !client.processing &&
               !client.throttled) {
            DecodeResult decoded = decodeFrame(client.input.view(), maxFrame);

            if (decoded.status == DecodeStatus::Incomplete) {
                // Once reading is over, a partial frame can never be
                // completed.
                if (client.state == State::ReadClosed)
                    client.input.clear();

                break;
            }

            if (decoded.status == DecodeStatus::Invalid) {
                // Without a usable length there is no telling where the
                // next frame starts. Answer and serve nothing more. If
                // the peer has already finished sending, the connection
                // closes once the answer is out.
                queueResponse(client, encodeError(0, decoded.error,
                                                  errorName(decoded.error)));

                client.input.clear();

                if (client.state == State::Open)
                    client.state = State::Closing;

                break;
            }

            client.input.consume(decoded.consumed);

            Frame& request = decoded.frame;
            ErrorCode error;

            if (!validateRequest(request, error)) {
                queueResponse(client, encodeError(request.requestId, error,
                                                  errorName(error)));
                continue;
            }

            // Cheap enough to answer on this thread. It still waited its
            // turn behind any request that was with a worker.
            if (request.type == TYPE_PING) {
                Frame response;
                response.type = TYPE_PING | RESPONSE_BIT;
                response.requestId = request.requestId;

                queueResponse(client, encodeFrame(response));
                continue;
            }

            client.processing = true;

            try {
                pool.enqueue([fd, id = client.id, request = std::move(request),
                              &completionMutex, &completions]() mutable {
                    Completion result{fd, id, "", false};

                    // An exception must still produce a completion, or
                    // the client would stay in the processing state
                    // forever.
                    try {
                        result.response = processRequest(std::move(request));
                    } catch (...) {
                        result.failed = true;
                    }

                    bool wasEmpty;

                    {
                        std::lock_guard<std::mutex> lock(completionMutex);

                        wasEmpty = completions.empty();
                        completions.push(std::move(result));
                    }

                    // A non-empty queue already has a wakeup on its way.
                    if (wasEmpty)
                        wake();
                });
            } catch (const std::exception& e) {
                std::cerr << "Task error: " << e.what() << '\n';

                client.processing = false;
                client.input.clear();
                client.state = State::Closing;
            }
        }
    };

    // The peer has closed and there is nothing left to decode, compute,
    // or send. Nothing is unread either, so close() ends the connection
    // with FIN and not with a reset.
    auto finished = [](const Client& client) {
        return client.state == State::ReadClosed && !client.processing &&
               client.input.size() == 0 && client.output.size() == 0;
    };

    std::cout << "Listening on 127.0.0.1:" << config.port << " with "
              << config.workers << " workers (Ctrl+C to stop)" << std::endl;

    // 4. Main event loop
    while (running) {

        // Collect responses completed by worker threads. The queue is
        // swapped out so the lock is not held while the responses are
        // handled, which can submit new work.
        std::queue<Completion> done;

        {
            std::lock_guard<std::mutex> lock(completionMutex);
            std::swap(done, completions);
        }

        while (!done.empty()) {
            Completion result = std::move(done.front());
            done.pop();

            auto it = clients.find(result.fd);

            if (it == clients.end() || it->second.id != result.id)
                continue;

            if (result.failed) {
                closeClient(result.fd);
                continue;
            }

            Client& client = it->second;

            client.processing = false;
            queueResponse(client, result.response);
            serveRequests(result.fd, client);
        }

        // 5. Build poll descriptor list, and on the way close connections
        // that have stayed in one phase for too long. The scan is linear,
        // which costs nothing extra while poll() is linear too.
        std::vector<pollfd> fds;
        fds.reserve(clients.size() + 2);

        fds.push_back({serverSocket, 0, 0});
        fds.push_back({wakePipe[0], POLLIN, 0});

        auto now = std::chrono::steady_clock::now();
        long long timeoutMs = acceptBackoff ? ACCEPT_RETRY_MS : -1;

        for (auto it = clients.begin(); it != clients.end();) {
            int fd = it->first;
            Client& client = it->second;

            Phase phase = phaseOf(client);

            if (phase != client.phase) {
                client.phase = phase;
                client.phaseSince = now;
            }

            int limitMs = phaseLimitMs[static_cast<int>(phase)];

            if (limitMs >= 0) {
                auto deadline =
                    client.phaseSince + std::chrono::milliseconds(limitMs);

                long long remainingMs =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        deadline - now)
                        .count();

                if (remainingMs <= 0) {
                    logLimited(lastTimeoutLog, "timeout", timeoutName(phase));
                    close(fd);
                    it = clients.erase(it);
                    continue;
                }

                // One more than the truncated remainder, so that poll()
                // returns after the deadline and not just before it.
                if (timeoutMs < 0 || remainingMs + 1 < timeoutMs)
                    timeoutMs = remainingMs + 1;
            }

            int events = 0;

            if (client.output.size() > 0)
                events |= POLLOUT;

            if (wantsRead(client))
                events |= POLLIN;

            fds.push_back({fd, static_cast<short>(events), 0});
            ++it;
        }

        // At the connection limit the listening socket is left out, so
        // new connections wait in the kernel backlog and poll() does not
        // keep returning for a socket we are not going to accept from.
        if (clients.size() < maxConnections && !acceptBackoff)
            fds[0].events = POLLIN;

        // 6. Sleep until a socket is ready, wake() is called, or the
        // nearest deadline arrives.
        int ready = poll(fds.data(), static_cast<nfds_t>(fds.size()),
                         static_cast<int>(timeoutMs));

        acceptBackoff = false;

        if (ready < 0) {
            if (errno == EINTR)
                continue;

            perror("poll");
            break;
        }

        now = std::chrono::steady_clock::now();

        // Empty the wake pipe. This must happen before the completion
        // queue is drained at the top of the next iteration, otherwise
        // a wakeup could be lost.
        if (fds[1].revents & POLLIN) {
            char drain[256];

            while (read(wakePipe[0], drain, sizeof(drain)) > 0) {
            }
        }

        // 7. Accept new connections
        if (fds[0].revents & POLLIN) {
            while (clients.size() < maxConnections) {
                int fd = accept(serverSocket, nullptr, nullptr);

                // Out of descriptors. On Linux the connection stays in
                // the backlog and keeps the listening socket readable, so
                // poll() would return at once, forever. Use the spare
                // descriptor's slot to accept the connection and close it.
                if (fd < 0 && (errno == EMFILE || errno == ENFILE)) {
                    logLimited(lastAcceptLog, "accept",
                               "out of file descriptors, "
                               "refusing new connections");

                    close(spareFd);

                    fd = accept(serverSocket, nullptr, nullptr);
                    int acceptErrno = errno;

                    if (fd >= 0)
                        close(fd);

                    spareFd = open("/dev/null", O_RDONLY);

                    if (fd >= 0)
                        continue;

                    errno = acceptErrno;
                }

                if (fd < 0) {
                    if (errno == EINTR || errno == ECONNABORTED)
                        continue;

                    // Anything else that is not "queue empty" may leave
                    // a connection in the backlog, so back off instead
                    // of polling the listening socket again right away.
                    if (errno != EAGAIN && errno != EWOULDBLOCK) {
                        logLimited(lastAcceptLog, "accept",
                                   std::strerror(errno));
                        acceptBackoff = true;
                    }

                    break;
                }

                if (!setNonBlocking(fd)) {
                    close(fd);
                    continue;
                }

                Client client;
                client.id = nextId++;
                client.phaseSince = now;

                clients.emplace(fd, std::move(client));
            }
        }

        // 8. Handle client socket events
        for (std::size_t i = 2; i < fds.size(); ++i) {
            int fd = fds[i].fd;

            auto it = clients.find(fd);

            if (it == clients.end())
                continue;

            Client& client = it->second;
            short revents = fds[i].revents;

            if (revents & (POLLERR | POLLNVAL)) {
                int error = 0;
                socklen_t length = sizeof(error);

                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) ==
                        0 &&
                    error != 0)
                    logLimited(lastConnectionLog, "connection",
                               std::strerror(error));

                closeClient(fd);
                continue;
            }

            bool reading = wantsRead(client);

            // POLLHUP says the peer will send no more. While we are still
            // reading, recv() below collects what is left and then
            // reports the end, and responses we owe can still go out.
            // When we are not reading, there is nothing left to learn
            // from this socket and nobody to write to.
            if ((revents & POLLHUP) && !reading) {
                closeClient(fd);
                continue;
            }

            bool failed = false;

            // Socket is ready for writing
            if (revents & POLLOUT) {
                while (client.output.size() > 0) {
                    ssize_t n =
                        send(fd, client.output.data(), client.output.size(), 0);

                    if (n > 0) {
                        client.output.consume(static_cast<std::size_t>(n));
                        client.phaseSince = now;
                    }
                    else if (n < 0 && errno == EINTR) {
                        continue;
                    }
                    else if (n < 0 &&
                             (errno == EAGAIN || errno == EWOULDBLOCK)) {
                        break;
                    }
                    else {
                        failed = true;
                        break;
                    }
                }

                if (client.output.size() <= outputLowWater)
                    client.throttled = false;
            }

            // Socket is ready for reading. Take everything that is there,
            // up to the input limit, before decoding any of it. In Closing
            // the bytes are dropped, so the limit only bounds one round.
            if (!failed && reading && (revents & (POLLIN | POLLHUP))) {
                char chunk[READ_CHUNK];

                std::size_t room = inputLimit;

                if (client.state == State::Open)
                    room -= client.input.size();

                while (room > 0) {
                    ssize_t n =
                        recv(fd, chunk, std::min(sizeof(chunk), room), 0);

                    if (n > 0) {
                        auto count = static_cast<std::size_t>(n);

                        if (client.state == State::Open)
                            client.input.append({chunk, count});

                        room -= count;
                    }
                    else if (n == 0) {
                        client.state = State::ReadClosed;
                        break;
                    }
                    else if (errno == EINTR) {
                        continue;
                    }
                    else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        break;
                    }
                    else {
                        failed = true;
                        break;
                    }
                }
            }

            if (failed) {
                closeClient(fd);
                continue;
            }

            // 9. Decode what arrived and hand requests to the Thread Pool
            serveRequests(fd, client);

            // The error has gone out. Send FIN now and keep the socket
            // open for reading, so that bytes the peer is still sending
            // do not turn our close() into a reset that could destroy
            // the error before the peer has read it.
            if (client.state == State::Closing && client.output.size() == 0 &&
                !client.finSent) {
                shutdown(fd, SHUT_WR);
                client.finSent = true;
            }

            if (finished(client))
                closeClient(fd);
        }
    }

    // 10. Shutdown
    for (const auto& [fd, client] : clients)
        close(fd);

    close(serverSocket);
    close(spareFd);

    // Workers may still call wake(), so stop them before closing the pipe.
    pool.shutdown();
    close(wakePipe[0]);
    close(wakePipe[1]);

    std::cout << "Server stopped\n";
    return 0;
}
