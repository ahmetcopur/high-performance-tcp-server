
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

struct Client {
    std::uint64_t id = 0;

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

    // Nothing more will be read. Either the peer finished sending, or a
    // bad length made the rest of the stream unusable.
    bool readClosed = false;
};

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
                  << "Usage: " << argv[0] << " [--port N] [--workers N]"
                  << " [--max-connections N] [--backlog N]"
                  << " [--max-frame N] [--output-high-water N]\n";
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

    auto closeClient = [&](int fd) {
        close(fd);
        clients.erase(fd);
    };

    // Set when accept() fails for a reason that retrying at once cannot
    // fix. The listening socket then sits out one poll() round.
    bool acceptBackoff = false;

    std::chrono::steady_clock::time_point lastAcceptLog;

    // A flood of failing connections must not become a flood of log lines.
    auto logAcceptError = [&](const char* message) {
        auto now = std::chrono::steady_clock::now();

        if (now - lastAcceptLog < std::chrono::seconds(1))
            return;

        lastAcceptLog = now;
        std::cerr << "accept: " << message << '\n';
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

        while (!client.processing && !client.throttled) {
            DecodeResult decoded = decodeFrame(client.input.view(), maxFrame);

            if (decoded.status == DecodeStatus::Incomplete) {
                // Once reading is over, a partial frame can never be
                // completed.
                if (client.readClosed)
                    client.input.clear();

                break;
            }

            if (decoded.status == DecodeStatus::Invalid) {
                // Without a usable length there is no telling where the
                // next frame starts. Answer, read nothing more, and let
                // the connection close once the output is sent.
                queueResponse(client, encodeError(0, decoded.error,
                                                  errorName(decoded.error)));

                client.input.clear();
                client.readClosed = true;
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
                client.readClosed = true;
            }
        }
    };

    // Nothing left to read, decode, compute, or send.
    auto finished = [](const Client& client) {
        return client.readClosed && !client.processing &&
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

        // 5. Build poll descriptor list
        std::vector<pollfd> fds;
        fds.reserve(clients.size() + 2);

        // At the connection limit the listening socket is left out, so
        // new connections wait in the kernel backlog and poll() does not
        // keep returning for a socket we are not going to accept from.
        short listenEvents = 0;

        if (clients.size() < maxConnections && !acceptBackoff)
            listenEvents = POLLIN;

        fds.push_back({serverSocket, listenEvents, 0});
        fds.push_back({wakePipe[0], POLLIN, 0});

        for (const auto& [fd, client] : clients) {
            int events = 0;

            if (client.output.size() > 0)
                events |= POLLOUT;

            if (!client.readClosed && !client.throttled &&
                client.input.size() < inputLimit)
                events |= POLLIN;

            fds.push_back({fd, static_cast<short>(events), 0});
        }

        // 6. Sleep until a socket is ready or wake() is called. During an
        // accept backoff, also wake up on a timer to try accept() again.
        int ready = poll(fds.data(), static_cast<nfds_t>(fds.size()),
                         acceptBackoff ? ACCEPT_RETRY_MS : -1);

        acceptBackoff = false;

        if (ready < 0) {
            if (errno == EINTR)
                continue;

            perror("poll");
            break;
        }

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
                    logAcceptError("out of file descriptors, "
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
                        logAcceptError(std::strerror(errno));
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

            if (revents & (POLLERR | POLLHUP | POLLNVAL)) {
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
            // up to the input limit, before decoding any of it.
            if (!failed && (revents & POLLIN)) {
                char chunk[READ_CHUNK];

                while (client.input.size() < inputLimit) {
                    std::size_t room = std::min(
                        sizeof(chunk), inputLimit - client.input.size());

                    ssize_t n = recv(fd, chunk, room, 0);

                    if (n > 0) {
                        client.input.append(
                            {chunk, static_cast<std::size_t>(n)});
                    }
                    else if (n == 0) {
                        client.readClosed = true;
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
