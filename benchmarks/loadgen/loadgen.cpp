// A load generator for the server. It opens many connections on several
// threads, sends requests, and measures how long the answers take.
//
// Two ways of sending:
//
//   Closed loop (default). Each connection sends a request, waits for
//   the answer, and sends the next. The load adapts to the server: when
//   the server slows down, the client sends less. That gives the highest
//   throughput the pair can reach, and latencies that look better than
//   what an independent client would see.
//
//   Open loop (--rate). Requests are due at fixed times whether or not
//   the server has caught up, like clients that do not know about each
//   other. Latency is counted from the time a request was due, not from
//   the time it was finally sent. A request that had to wait behind a
//   slow one therefore carries that wait. Leaving it out is known as
//   coordinated omission, and it hides exactly the latencies that
//   matter.
//
// The result is written as JSON. See benchmarks/README.md.

#include "Buffer.h"
#include "Config.h"
#include "FileDescriptor.h"
#include "Histogram.h"
#include "Poller.h"
#include "Protocol.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

namespace {

using Clock = std::chrono::steady_clock;

struct Options {
    std::string host = "127.0.0.1";
    int port = 8080;

    int connections = 10;
    int idleConnections = 0;
    int threads = 1;

    double warmupSeconds = 1.0;
    double durationSeconds = 5.0;

    std::string type = "echo";
    int payloadBytes = 64;
    int iterations = 1000;

    // Requests per second over all connections. 0 means closed loop.
    double rate = 0.0;

    int timeoutMs = 5000;
    std::string output;
    PollerKind poller = Config().poller;
};

const char* const USAGE =
    "Usage: loadgen [option value]...\n"
    "  --host A              server address (default 127.0.0.1)\n"
    "  --port N              server port (default 8080)\n"
    "  --connections N       connections that send requests (default 10)\n"
    "  --idle-connections N  extra connections that send nothing (default 0)\n"
    "  --threads N           client threads (default 1)\n"
    "  --warmup S            seconds to run before measuring (default 1)\n"
    "  --duration S          seconds to measure (default 5)\n"
    "  --type T              ping, echo or compute (default echo)\n"
    "  --payload N           bytes of payload or of compute data (default 64)\n"
    "  --iterations N        compute iterations (default 1000)\n"
    "  --rate R              requests per second in total; 0 for closed loop\n"
    "  --timeout-ms N        give up on a request after this long (default "
    "5000)\n"
    "  --poller P            poll or epoll\n"
    "  --output FILE         write the JSON result here, not to stdout\n";

bool parseNumber(const char* text, int& out) {
    const char* end = text + std::strlen(text);
    auto result = std::from_chars(text, end, out);

    return result.ec == std::errc() && result.ptr == end;
}

bool parseNumber(const char* text, double& out) {
    char* end = nullptr;
    out = std::strtod(text, &end);

    return end != text && *end == '\0';
}

bool parseOptions(int argc, char** argv, Options& options, std::string& error) {
    for (int i = 1; i < argc; i += 2) {
        std::string name = argv[i];

        if (i + 1 >= argc) {
            error = name + " needs a value";
            return false;
        }

        const char* value = argv[i + 1];
        bool ok = true;

        if (name == "--host")
            options.host = value;
        else if (name == "--port")
            ok = parseNumber(value, options.port) && options.port > 0;
        else if (name == "--connections")
            ok = parseNumber(value, options.connections) &&
                 options.connections > 0;
        else if (name == "--idle-connections")
            ok = parseNumber(value, options.idleConnections) &&
                 options.idleConnections >= 0;
        else if (name == "--threads")
            ok = parseNumber(value, options.threads) && options.threads > 0;
        else if (name == "--warmup")
            ok = parseNumber(value, options.warmupSeconds) &&
                 options.warmupSeconds >= 0;
        else if (name == "--duration")
            ok = parseNumber(value, options.durationSeconds) &&
                 options.durationSeconds > 0;
        else if (name == "--type")
            ok = (options.type = value) == "ping" || options.type == "echo" ||
                 options.type == "compute";
        else if (name == "--payload")
            ok = parseNumber(value, options.payloadBytes) &&
                 options.payloadBytes >= 0;
        else if (name == "--iterations")
            ok = parseNumber(value, options.iterations) &&
                 options.iterations >= 0;
        else if (name == "--rate")
            ok = parseNumber(value, options.rate) && options.rate >= 0;
        else if (name == "--timeout-ms")
            ok = parseNumber(value, options.timeoutMs) && options.timeoutMs > 0;
        else if (name == "--output")
            options.output = value;
        else if (name == "--poller") {
            Config config;
            const char* arguments[] = {"loadgen", "--poller", value};

            ok = parseConfig(3, arguments, config, error);
            options.poller = config.poller;

            if (!ok)
                return false;
        }
        else {
            error = "unknown option: " + name;
            return false;
        }

        if (!ok) {
            error = "invalid value for " + name + ": " + value;
            return false;
        }
    }

    if (options.threads > options.connections)
        options.threads = options.connections;

    return true;
}

// What every request of a run has in common.
struct Workload {
    std::uint8_t type = 0;

    // The payload. The header in front of it differs only in the id.
    std::string body;

    // What a correct answer looks like.
    std::uint8_t responseType = 0;
    std::size_t responsePayloadBytes = 0;

    explicit Workload(const Options& options) {
        using namespace protocol;

        if (options.type == "ping") {
            type = TYPE_PING;
        }
        else if (options.type == "echo") {
            type = TYPE_ECHO;
            body.assign(static_cast<std::size_t>(options.payloadBytes), 'x');
            responsePayloadBytes = body.size();
        }
        else {
            type = TYPE_COMPUTE;
            appendU32(body, static_cast<std::uint32_t>(options.iterations));
            body.append(static_cast<std::size_t>(options.payloadBytes), 'x');
            responsePayloadBytes = 8;
        }

        responseType = static_cast<std::uint8_t>(type | RESPONSE_BIT);
    }

    std::size_t requestBytes() const {
        return protocol::HEADER_SIZE + body.size();
    }
};

struct Counters {
    Histogram latency;

    std::uint64_t completed = 0;        // answered correctly
    std::uint64_t busy = 0;             // answered with ERROR 5
    std::uint64_t errors = 0;           // any other wrong answer
    std::uint64_t timeouts = 0;         // no answer in time
    std::uint64_t connectionErrors = 0; // connection lost
    std::uint64_t unfinished = 0;       // still waiting when the run ended
    std::uint64_t notSent = 0;          // due, but never sent (open loop)

    std::uint64_t bytesSent = 0;
    std::uint64_t bytesReceived = 0;

    std::uint64_t connected = 0;
    std::uint64_t connectFailures = 0;
    std::uint64_t idleLost = 0;

    // Open loop: how the client kept to its own schedule. A request is
    // late when the client got round to it more than a millisecond after
    // it was due, which is the client's fault and not the server's.
    std::uint64_t scheduled = 0;
    std::uint64_t scheduledLate = 0;
    std::uint64_t worstLateNs = 0;

    void add(const Counters& other) {
        latency.merge(other.latency);
        completed += other.completed;
        busy += other.busy;
        errors += other.errors;
        timeouts += other.timeouts;
        connectionErrors += other.connectionErrors;
        unfinished += other.unfinished;
        notSent += other.notSent;
        bytesSent += other.bytesSent;
        bytesReceived += other.bytesReceived;
        connected += other.connected;
        connectFailures += other.connectFailures;
        idleLost += other.idleLost;
        scheduled += other.scheduled;
        scheduledLate += other.scheduledLate;
        worstLateNs = std::max(worstLateNs, other.worstLateNs);
    }
};

// Lets every thread finish connecting before any of them starts sending,
// so that connection setup is not part of what is measured.
struct StartLine {
    std::mutex mutex;
    std::condition_variable changed;
    int ready = 0;
    bool go = false;
    Clock::time_point start;
};

struct Connection {
    FileDescriptor socket;
    Buffer input;
    bool idle = false;
    bool dead = false;

    // A request has been started and its answer has not arrived.
    bool waiting = false;

    // When latency for the current request started to count: when it was
    // sent in a closed loop, when it was due in an open loop.
    Clock::time_point countedFrom;
    std::uint32_t requestId = 0;
    char header[protocol::HEADER_SIZE];

    // Bytes of the current request already written.
    std::size_t sent = 0;
    bool wantsWrite = false;

    // Open loop: times at which requests fell due while this connection
    // was still waiting for an earlier answer.
    std::deque<Clock::time_point> due;
};

class Worker {
private:
    const Options& options;
    const Workload& workload;
    StartLine& startLine;
    int index;

    std::unique_ptr<Poller> poller;
    std::vector<std::unique_ptr<Connection>> connections;
    std::unordered_map<int, Connection*> byFd;
    std::vector<Connection*> active;

    Clock::time_point measureFrom;
    Clock::time_point end;
    bool measuring = false;

    bool connect(Connection& connection) {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<std::uint16_t>(options.port));

        if (inet_pton(AF_INET, options.host.c_str(), &address.sin_addr) != 1)
            return false;

        connection.socket.reset(socket(AF_INET, SOCK_STREAM, 0));

        if (!connection.socket.valid())
            return false;

        int fd = connection.socket.get();

        // Blocking, one at a time per thread, so the server's backlog is
        // never asked to hold more than a few.
        if (::connect(fd, reinterpret_cast<sockaddr*>(&address),
                      sizeof(address)) < 0)
            return false;

        // Each request is one write, so there is nothing for the kernel
        // to gain by holding small packets back.
        int on = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));

        return connection.socket.setNonBlocking() &&
               poller->add(fd, true, false);
    }

    void lose(Connection& connection) {
        if (connection.dead)
            return;

        connection.dead = true;

        if (connection.idle) {
            ++counters.idleLost;
        }
        else {
            if (connection.waiting && measuring)
                ++counters.connectionErrors;

            if (measuring)
                counters.notSent += connection.due.size();
        }

        poller->remove(connection.socket.get());
        byFd.erase(connection.socket.get());
        connection.socket.reset();
    }

    // Writes as much of the current request as the socket takes.
    void sendSome(Connection& connection) {
        std::size_t total = workload.requestBytes();

        while (connection.sent < total) {
            iovec parts[2];
            int count = 0;

            if (connection.sent < protocol::HEADER_SIZE) {
                parts[count].iov_base = connection.header + connection.sent;
                parts[count].iov_len = protocol::HEADER_SIZE - connection.sent;
                ++count;

                if (!workload.body.empty()) {
                    parts[count].iov_base =
                        const_cast<char*>(workload.body.data());
                    parts[count].iov_len = workload.body.size();
                    ++count;
                }
            }
            else {
                std::size_t offset = connection.sent - protocol::HEADER_SIZE;

                parts[count].iov_base =
                    const_cast<char*>(workload.body.data()) + offset;
                parts[count].iov_len = workload.body.size() - offset;
                ++count;
            }

            ssize_t n = writev(connection.socket.get(), parts, count);

            if (n > 0) {
                connection.sent += static_cast<std::size_t>(n);

                if (measuring)
                    counters.bytesSent += static_cast<std::uint64_t>(n);
            }
            else if (n < 0 && errno == EINTR) {
                continue;
            }
            else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                break;
            }
            else {
                lose(connection);
                return;
            }
        }

        bool wantsWrite = connection.sent < total;

        if (wantsWrite != connection.wantsWrite) {
            connection.wantsWrite = wantsWrite;
            poller->modify(connection.socket.get(), true, wantsWrite);
        }
    }

    void startRequest(Connection& connection, Clock::time_point countedFrom) {
        using namespace protocol;

        std::string header;
        appendU32(header, static_cast<std::uint32_t>(MIN_LENGTH +
                                                     workload.body.size()));
        header.push_back(static_cast<char>(VERSION));
        header.push_back(static_cast<char>(workload.type));
        appendU16(header, 0);
        appendU32(header, ++connection.requestId);

        std::memcpy(connection.header, header.data(), HEADER_SIZE);

        connection.sent = 0;
        connection.waiting = true;
        connection.countedFrom = countedFrom;

        sendSome(connection);
    }

    // The next thing a connection does once it is free.
    void next(Connection& connection, Clock::time_point now) {
        if (connection.dead || now >= end)
            return;

        if (options.rate > 0) {
            if (connection.due.empty())
                return;

            Clock::time_point due = connection.due.front();
            connection.due.pop_front();
            startRequest(connection, due);
        }
        else {
            startRequest(connection, now);
        }
    }

    void receive(Connection& connection) {
        using namespace protocol;

        char chunk[256 * 1024];

        while (true) {
            ssize_t n = recv(connection.socket.get(), chunk, sizeof(chunk), 0);

            if (n > 0) {
                if (measuring)
                    counters.bytesReceived += static_cast<std::uint64_t>(n);

                if (!connection.idle)
                    connection.input.append(
                        {chunk, static_cast<std::size_t>(n)});

                if (static_cast<std::size_t>(n) < sizeof(chunk))
                    break;
            }
            else if (n < 0 && errno == EINTR) {
                continue;
            }
            else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                break;
            }
            else {
                lose(connection);
                return;
            }
        }

        // Answers are taken apart here by hand, so that a large payload
        // is not copied a second time just to be looked at.
        while (!connection.dead && connection.input.size() >= HEADER_SIZE) {
            const char* bytes = connection.input.data();
            std::uint32_t length = readU32(bytes);

            if (length < MIN_LENGTH) {
                if (measuring)
                    ++counters.errors;

                lose(connection);
                return;
            }

            std::size_t frameBytes = LENGTH_SIZE + length;

            if (connection.input.size() < frameBytes)
                break;

            auto type = static_cast<std::uint8_t>(bytes[5]);
            std::uint32_t requestId = readU32(bytes + 8);
            std::string_view payload(bytes + HEADER_SIZE, length - MIN_LENGTH);

            // Everything that is needed from the answer is noted before
            // the buffer lets go of its bytes.
            Answer answer;
            answer.type = type;
            answer.requestId = requestId;
            answer.payloadBytes = payload.size();
            answer.busy = type == TYPE_ERROR && payload.size() >= 2 &&
                          readU16(payload.data()) ==
                              static_cast<std::uint16_t>(ErrorCode::ServerBusy);

            connection.input.consume(frameBytes);

            if (!connection.waiting) {
                // An answer nobody asked for.
                if (measuring)
                    ++counters.errors;

                continue;
            }

            finishRequest(connection, answer);
        }
    }

    struct Answer {
        std::uint8_t type = 0;
        std::uint32_t requestId = 0;
        std::size_t payloadBytes = 0;
        bool busy = false;
    };

    void finishRequest(Connection& connection, const Answer& answer) {
        Clock::time_point now = Clock::now();
        bool counts = now >= measureFrom && now < end;

        connection.waiting = false;

        if (counts) {
            if (answer.requestId != connection.requestId) {
                ++counters.errors;
            }
            else if (answer.type == workload.responseType &&
                     answer.payloadBytes == workload.responsePayloadBytes) {
                ++counters.completed;
                counters.latency.record(static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        now - connection.countedFrom)
                        .count()));
            }
            else if (answer.busy) {
                ++counters.busy;
            }
            else {
                ++counters.errors;
            }
        }

        next(connection, now);
    }

    void checkTimeouts(Clock::time_point now) {
        auto limit = std::chrono::milliseconds(options.timeoutMs);

        for (Connection* connection : active) {
            if (connection->dead || !connection->waiting)
                continue;

            // Counted from when the request was really sent or due,
            // whichever the latency is counted from.
            if (now - connection->countedFrom < limit)
                continue;

            if (measuring)
                ++counters.timeouts;

            connection->waiting = false;
            lose(*connection);
        }
    }

public:
    Counters counters;

    Worker(const Options& runOptions, const Workload& runWorkload,
           StartLine& line, int workerIndex)
        : options(runOptions), workload(runWorkload), startLine(line),
          index(workerIndex), poller(makePoller(runOptions.poller)) {
    }

    void run() {
        // This thread's share of the connections.
        for (int i = index; i < options.connections + options.idleConnections;
             i += options.threads) {
            auto connection = std::make_unique<Connection>();
            connection->idle = i >= options.connections;

            if (!connect(*connection)) {
                ++counters.connectFailures;
                continue;
            }

            ++counters.connected;
            byFd[connection->socket.get()] = connection.get();

            if (!connection->idle)
                active.push_back(connection.get());

            connections.push_back(std::move(connection));
        }

        Clock::time_point start;

        {
            std::unique_lock<std::mutex> lock(startLine.mutex);

            ++startLine.ready;
            startLine.changed.notify_all();

            startLine.changed.wait(lock, [this]() {
                return startLine.go;
            });

            start = startLine.start;
        }

        measureFrom =
            start + std::chrono::duration_cast<Clock::duration>(
                        std::chrono::duration<double>(options.warmupSeconds));
        end = measureFrom +
              std::chrono::duration_cast<Clock::duration>(
                  std::chrono::duration<double>(options.durationSeconds));

        std::this_thread::sleep_until(start);

        // Open loop: this thread's share of the rate, and the first time
        // a request falls due. Threads are staggered so that they do not
        // all fire at the same instants.
        Clock::duration interval{};
        Clock::time_point nextDue = end;
        std::size_t turn = 0;

        if (options.rate > 0 && !active.empty()) {
            double share = options.rate * static_cast<double>(active.size()) /
                           static_cast<double>(options.connections);

            interval = std::chrono::duration_cast<Clock::duration>(
                std::chrono::duration<double>(1.0 / share));
            nextDue = start + interval * index / options.threads;
        }
        else {
            for (Connection* connection : active)
                next(*connection, start);
        }

        std::vector<PollEvent> events;
        Clock::time_point lastTimeoutCheck = start;

        while (true) {
            Clock::time_point now = Clock::now();

            if (now >= end)
                break;

            measuring = now >= measureFrom;

            // Requests fall due on schedule whether or not the connection
            // they belong to is free.
            while (options.rate > 0 && nextDue <= now) {
                Connection& connection = *active[turn++ % active.size()];

                if (measuring) {
                    auto lateNs = static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            now - nextDue)
                            .count());

                    ++counters.scheduled;
                    counters.worstLateNs =
                        std::max(counters.worstLateNs, lateNs);

                    if (lateNs > 1000000)
                        ++counters.scheduledLate;
                }

                if (connection.dead) {
                    if (measuring)
                        ++counters.notSent;
                }
                else if (connection.waiting) {
                    connection.due.push_back(nextDue);
                }
                else {
                    startRequest(connection, nextDue);
                }

                nextDue += interval;
            }

            if (now - lastTimeoutCheck >= std::chrono::milliseconds(100)) {
                lastTimeoutCheck = now;
                checkTimeouts(now);
            }

            Clock::time_point wakeAt =
                std::min({end, nextDue,
                          lastTimeoutCheck + std::chrono::milliseconds(100)});

            auto waitMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                              wakeAt - now)
                              .count();

            // Rounded down, so that a request is not sent late because
            // the wait overshot. The loop comes round again instead, which
            // means it spins for the last fraction of a millisecond before
            // a request is due. In an open loop the client's CPU time
            // therefore says little, and its schedule is what to look at.
            if (!poller->wait(static_cast<int>(std::max<long long>(waitMs, 0)),
                              events))
                break;

            for (const PollEvent& event : events) {
                auto found = byFd.find(event.fd);

                if (found == byFd.end())
                    continue;

                Connection& connection = *found->second;

                if (event.error) {
                    lose(connection);
                    continue;
                }

                if (event.writable && connection.waiting)
                    sendSome(connection);

                if (!connection.dead && (event.readable || event.hangup))
                    receive(connection);
            }
        }

        for (Connection* connection : active) {
            if (connection->dead)
                continue;

            if (connection->waiting)
                ++counters.unfinished;

            counters.notSent += connection->due.size();
        }
    }
};

double cpuSeconds(const timeval& time) {
    return static_cast<double>(time.tv_sec) +
           static_cast<double>(time.tv_usec) / 1e6;
}

// A percentile in the JSON, or null if there are too few samples for it
// to mean anything.
std::string percentileJson(const Histogram& histogram, double fraction) {
    if (!histogram.supports(fraction))
        return "null";

    return std::to_string(histogram.percentile(fraction));
}

std::string resultJson(const Options& options, const Workload& workload,
                       const Counters& total, double measuredSeconds,
                       double cpuUser, double cpuSystem) {
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(3);

    const Histogram& latency = total.latency;
    double utilization =
        (cpuUser + cpuSystem) /
        (measuredSeconds * static_cast<double>(options.threads));

    out << "{\n";
    out << "  \"config\": {\n";
    out << "    \"host\": \"" << options.host << "\",\n";
    out << "    \"port\": " << options.port << ",\n";
    out << "    \"connections\": " << options.connections << ",\n";
    out << "    \"idle_connections\": " << options.idleConnections << ",\n";
    out << "    \"threads\": " << options.threads << ",\n";
    out << "    \"warmup_s\": " << options.warmupSeconds << ",\n";
    out << "    \"duration_s\": " << options.durationSeconds << ",\n";
    out << "    \"type\": \"" << options.type << "\",\n";
    out << "    \"payload_bytes\": " << options.payloadBytes << ",\n";
    out << "    \"iterations\": " << options.iterations << ",\n";
    out << "    \"mode\": \"" << (options.rate > 0 ? "open" : "closed")
        << "\",\n";
    out << "    \"rate_target_rps\": " << options.rate << ",\n";
    out << "    \"timeout_ms\": " << options.timeoutMs << ",\n";
    out << "    \"poller\": \"" << pollerName(options.poller) << "\",\n";
    out << "    \"request_bytes\": " << workload.requestBytes() << ",\n";
    out << "    \"response_bytes\": "
        << protocol::HEADER_SIZE + workload.responsePayloadBytes << "\n";
    out << "  },\n";

    out << "  \"measured_s\": " << measuredSeconds << ",\n";

    out << "  \"connections\": {\n";
    out << "    \"connected\": " << total.connected << ",\n";
    out << "    \"connect_failures\": " << total.connectFailures << ",\n";
    out << "    \"idle_lost\": " << total.idleLost << "\n";
    out << "  },\n";

    out << "  \"requests\": {\n";
    out << "    \"completed\": " << total.completed << ",\n";
    out << "    \"busy\": " << total.busy << ",\n";
    out << "    \"errors\": " << total.errors << ",\n";
    out << "    \"timeouts\": " << total.timeouts << ",\n";
    out << "    \"connection_errors\": " << total.connectionErrors << ",\n";
    out << "    \"unfinished\": " << total.unfinished << ",\n";
    out << "    \"not_sent\": " << total.notSent << "\n";
    out << "  },\n";

    out << "  \"throughput_rps\": "
        << static_cast<double>(total.completed) / measuredSeconds << ",\n";

    out << "  \"bytes\": {\n";
    out << "    \"sent\": " << total.bytesSent << ",\n";
    out << "    \"received\": " << total.bytesReceived << ",\n";
    out << "    \"sent_per_s\": "
        << static_cast<double>(total.bytesSent) / measuredSeconds << ",\n";
    out << "    \"received_per_s\": "
        << static_cast<double>(total.bytesReceived) / measuredSeconds << "\n";
    out << "  },\n";

    out << "  \"latency_ns\": {\n";
    out << "    \"counted_from\": \""
        << (options.rate > 0 ? "when the request was due"
                             : "when the request was sent")
        << "\",\n";
    out << "    \"count\": " << latency.count() << ",\n";
    out << "    \"min\": " << latency.min() << ",\n";
    out << "    \"mean\": " << latency.mean() << ",\n";
    out << "    \"p50\": " << percentileJson(latency, 0.50) << ",\n";
    out << "    \"p90\": " << percentileJson(latency, 0.90) << ",\n";
    out << "    \"p95\": " << percentileJson(latency, 0.95) << ",\n";
    out << "    \"p99\": " << percentileJson(latency, 0.99) << ",\n";
    out << "    \"p999\": " << percentileJson(latency, 0.999) << ",\n";
    out << "    \"max\": " << latency.max() << "\n";
    out << "  },\n";

    // Every bucket that has samples, as [upper bound in ns, count], so
    // that any other percentile can be worked out later.
    out << "  \"histogram\": [";
    bool first = true;

    latency.forEachBucket([&](std::uint64_t top, std::uint64_t count) {
        out << (first ? "" : ",") << "[" << top << "," << count << "]";
        first = false;
    });

    out << "],\n";

    // In a closed loop the client is the limit when its threads have no
    // idle time left. In an open loop it is the limit when it cannot keep
    // to its own schedule.
    bool saturated = options.rate > 0
                         ? total.scheduledLate * 100 > total.scheduled
                         : utilization >= 0.85;

    out << "  \"client\": {\n";
    out << "    \"cpu_user_s\": " << cpuUser << ",\n";
    out << "    \"cpu_system_s\": " << cpuSystem << ",\n";
    out << "    \"cpu_per_thread\": " << utilization << ",\n";
    out << "    \"scheduled\": " << total.scheduled << ",\n";
    out << "    \"scheduled_over_1ms_late\": " << total.scheduledLate << ",\n";
    out << "    \"worst_late_ns\": " << total.worstLateNs << ",\n";
    out << "    \"saturated\": " << (saturated ? "true" : "false") << "\n";
    out << "  }\n";
    out << "}\n";

    return out.str();
}

std::string milliseconds(std::uint64_t nanoseconds) {
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(3);
    out << static_cast<double>(nanoseconds) / 1e6 << " ms";
    return out.str();
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    std::string error;

    if (!parseOptions(argc, argv, options, error)) {
        std::cerr << error << "\n" << USAGE;
        return 2;
    }

    // Writing to a connection the server has closed must fail with an
    // error, not end the process.
    std::signal(SIGPIPE, SIG_IGN);

    Workload workload(options);
    StartLine startLine;

    std::vector<std::unique_ptr<Worker>> workers;
    std::vector<std::thread> threads;

    for (int i = 0; i < options.threads; ++i)
        workers.push_back(
            std::make_unique<Worker>(options, workload, startLine, i));

    for (int i = 0; i < options.threads; ++i)
        threads.emplace_back([&workers, i]() {
            workers[static_cast<std::size_t>(i)]->run();
        });

    Clock::time_point start;

    {
        std::unique_lock<std::mutex> lock(startLine.mutex);

        startLine.changed.wait(lock, [&]() {
            return startLine.ready == options.threads;
        });

        // A little in the future, so that every thread starts at the
        // same moment.
        start = Clock::now() + std::chrono::milliseconds(100);
        startLine.start = start;
        startLine.go = true;
    }

    startLine.changed.notify_all();

    auto measureFrom =
        start + std::chrono::duration_cast<Clock::duration>(
                    std::chrono::duration<double>(options.warmupSeconds));
    auto end = measureFrom +
               std::chrono::duration_cast<Clock::duration>(
                   std::chrono::duration<double>(options.durationSeconds));

    // The client's own CPU time over the measured part, to tell whether
    // the client or the server was the limit.
    rusage before{};
    rusage after{};

    std::this_thread::sleep_until(measureFrom);
    getrusage(RUSAGE_SELF, &before);
    Clock::time_point measuredFrom = Clock::now();

    std::this_thread::sleep_until(end);
    getrusage(RUSAGE_SELF, &after);
    Clock::time_point measuredTo = Clock::now();

    for (std::thread& thread : threads)
        thread.join();

    Counters total;

    for (const std::unique_ptr<Worker>& worker : workers)
        total.add(worker->counters);

    double measuredSeconds =
        std::chrono::duration<double>(measuredTo - measuredFrom).count();
    double cpuUser = cpuSeconds(after.ru_utime) - cpuSeconds(before.ru_utime);
    double cpuSystem = cpuSeconds(after.ru_stime) - cpuSeconds(before.ru_stime);

    std::string json = resultJson(options, workload, total, measuredSeconds,
                                  cpuUser, cpuSystem);

    if (options.output.empty()) {
        std::cout << json;
    }
    else {
        std::ofstream file(options.output);
        file << json;

        if (!file) {
            std::cerr << "cannot write " << options.output << "\n";
            return 1;
        }
    }

    const Histogram& latency = total.latency;

    std::cerr << total.completed << " requests in " << measuredSeconds << " s, "
              << static_cast<double>(total.completed) / measuredSeconds
              << " per second, " << total.connected << " connections\n";

    if (latency.count() > 0)
        std::cerr << "latency: p50 " << milliseconds(latency.percentile(0.5))
                  << ", p99 "
                  << (latency.supports(0.99)
                          ? milliseconds(latency.percentile(0.99))
                          : std::string("too few samples"))
                  << ", max " << milliseconds(latency.max()) << "\n";

    std::uint64_t failed =
        total.busy + total.errors + total.timeouts + total.connectionErrors;

    if (failed > 0 || total.connectFailures > 0)
        std::cerr << "not answered correctly: " << failed << " (busy "
                  << total.busy << ", errors " << total.errors << ", timeouts "
                  << total.timeouts << ", connections lost "
                  << total.connectionErrors
                  << "), could not connect: " << total.connectFailures << "\n";

    return 0;
}
