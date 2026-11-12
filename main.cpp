
#include "ThreadPool.h"

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <poll.h>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

constexpr int DEFAULT_PORT = 8080;
constexpr int DEFAULT_WORKERS = 8;
constexpr int BUFFER_SIZE = 4096;

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
    std::uint64_t id;
    bool processing = false;
    std::string output;
    std::size_t sent = 0;
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

    return flags >= 0 &&
           fcntl(fd, F_SETFL, flags | O_NONBLOCK) >= 0;
}

// Runs inside a worker thread
std::string processMessage(std::string message) {
    // Currently echoes the message.
    // Later we can add parsing or CPU-intensive processing.
    return message;
}

int main(int argc, char** argv) {
    // Usage: ./server [port] [workers]
    int port = argc > 1 ? std::atoi(argv[1]) : DEFAULT_PORT;
    int numWorkers = argc > 2 ? std::atoi(argv[2]) : DEFAULT_WORKERS;

    if (port <= 0 || port > 65535 || numWorkers <= 0) {
        std::cerr << "Usage: " << argv[0] << " [port] [workers]\n";
        return 1;
    }

    int wakePipe[2];

    if (pipe(wakePipe) < 0 ||
        !setNonBlocking(wakePipe[0]) ||
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
    address.sin_port = htons(static_cast<std::uint16_t>(port));

    // 2. Configure server
    if (setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR,
                   &opt, sizeof(opt)) < 0 ||
        !setNonBlocking(serverSocket) ||
        bind(serverSocket,
             reinterpret_cast<sockaddr*>(&address),
             sizeof(address)) < 0 ||
        listen(serverSocket, 128) < 0) {

        perror("server setup");
        close(serverSocket);
        return 1;
    }

    // 3. Create Thread Pool and shared result queue
    std::mutex completionMutex;
    std::queue<Completion> completions;

    ThreadPool pool(static_cast<std::size_t>(numWorkers));

    std::unordered_map<int, Client> clients;
    std::uint64_t nextId = 1;

    auto closeClient = [&](int fd) {
        close(fd);
        clients.erase(fd);
    };

    std::cout << "Listening on 127.0.0.1:" << port
              << " with " << numWorkers
              << " workers (Ctrl+C to stop)" << std::endl;

    // 4. Main event loop
    while (running) {

        // Collect responses completed by worker threads
        {
            std::lock_guard<std::mutex> lock(completionMutex);

            while (!completions.empty()) {
                Completion result =
                    std::move(completions.front());

                completions.pop();

                auto it = clients.find(result.fd);

                if (it == clients.end() ||
                    it->second.id != result.id)
                    continue;

                if (result.failed) {
                    closeClient(result.fd);
                    continue;
                }

                it->second.output = std::move(result.response);
                it->second.sent = 0;
                it->second.processing = false;
            }
        }

        // 5. Build poll descriptor list
        std::vector<pollfd> fds;
        fds.reserve(clients.size() + 2);

        fds.push_back({serverSocket, POLLIN, 0});
        fds.push_back({wakePipe[0], POLLIN, 0});

        for (const auto& [fd, client] : clients) {
            short events = 0;

            if (!client.output.empty())
                events = POLLOUT;
            else if (!client.processing)
                events = POLLIN;

            fds.push_back({fd, events, 0});
        }

        // 6. Sleep until a socket is ready or wake() is called
        int ready = poll(
            fds.data(),
            static_cast<nfds_t>(fds.size()),
            -1
        );

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
            while (true) {
                int fd = accept(
                    serverSocket, nullptr, nullptr
                );

                if (fd < 0) {
                    if (errno == EINTR)
                        continue;

                    if (errno != EAGAIN &&
                        errno != EWOULDBLOCK)
                        perror("accept");

                    break;
                }

                if (!setNonBlocking(fd)) {
                    close(fd);
                    continue;
                }

                clients.emplace(
                    fd, Client{nextId++, false, "", 0}
                );
            }
        }

        // 8. Handle client socket events
        for (std::size_t i = 2; i < fds.size(); ++i) {
            int fd = fds[i].fd;

            auto it = clients.find(fd);

            if (it == clients.end())
                continue;

            short revents = fds[i].revents;

            if (revents & (POLLERR | POLLHUP | POLLNVAL)) {
                closeClient(fd);
                continue;
            }

            // Socket is ready for writing
            if (revents & POLLOUT) {
                Client& client = it->second;
                bool failed = false;

                while (client.sent < client.output.size()) {
                    ssize_t n = send(
                        fd,
                        client.output.data() + client.sent,
                        client.output.size() - client.sent,
                        0
                    );

                    if (n > 0) {
                        client.sent +=
                            static_cast<std::size_t>(n);
                    }
                    else if (n < 0 && errno == EINTR) {
                        continue;
                    }
                    else if (n < 0 &&
                            (errno == EAGAIN ||
                             errno == EWOULDBLOCK)) {
                        break;
                    }
                    else {
                        failed = true;
                        break;
                    }
                }

                if (failed) {
                    closeClient(fd);
                }
                else if (client.sent == client.output.size()) {
                    client.output.clear();
                    client.sent = 0;
                }
            }

            // Socket is ready for reading
            else if (revents & POLLIN) {
                char buffer[BUFFER_SIZE];

                ssize_t n = recv(
                    fd, buffer, sizeof(buffer), 0
                );

                if (n == 0) {
                    closeClient(fd);
                }
                else if (n < 0) {
                    if (errno != EINTR &&
                        errno != EAGAIN &&
                        errno != EWOULDBLOCK) {
                        closeClient(fd);
                    }
                }
                else {
                    std::uint64_t id = it->second.id;
                    it->second.processing = true;

                    std::string message(
                        buffer, static_cast<std::size_t>(n)
                    );

                    // 9. Submit processing to Thread Pool
                    try {
                        pool.enqueue(
                            [fd, id,
                             message = std::move(message),
                             &completionMutex,
                             &completions]() mutable {

                                Completion result{fd, id, "", false};

                                // An exception must still produce a
                                // completion, or the client would stay
                                // in the processing state forever.
                                try {
                                    result.response = processMessage(
                                        std::move(message)
                                    );
                                }
                                catch (...) {
                                    result.failed = true;
                                }

                                bool wasEmpty;

                                {
                                    std::lock_guard<std::mutex>
                                        lock(completionMutex);

                                    wasEmpty = completions.empty();
                                    completions.push(std::move(result));
                                }

                                // A non-empty queue already has a
                                // wakeup on its way.
                                if (wasEmpty)
                                    wake();
                            }
                        );
                    }
                    catch (const std::exception& e) {
                        std::cerr << "Task error: "
                                  << e.what() << '\n';
                        closeClient(fd);
                    }
                }
            }
        }
    }

    // 10. Shutdown
    for (const auto& [fd, client] : clients)
        close(fd);

    close(serverSocket);

    // Workers may still call wake(), so stop them before closing the pipe.
    pool.shutdown();
    close(wakePipe[0]);
    close(wakePipe[1]);

    std::cout << "Server stopped\n";
    return 0;
}
