#include "Config.h"

#include <charconv>
#include <climits>
#include <cstring>

namespace {

bool parseInt(const char* text, int min, int max, int& out) {
    const char* end = text + std::strlen(text);
    int value = 0;

    auto result = std::from_chars(text, end, value);

    if (result.ec != std::errc() || result.ptr != end || value < min ||
        value > max)
        return false;

    out = value;
    return true;
}

struct Option {
    const char* name;
    int Config::* field;
    int min;
    int max;
};

const Option OPTIONS[] = {
    {"--port", &Config::port, 1, 65535},
    {"--workers", &Config::workers, 1, INT_MAX},
    {"--max-connections", &Config::maxConnections, 1, INT_MAX},
    {"--backlog", &Config::backlog, 1, INT_MAX},
    {"--max-frame", &Config::maxFrame, 8, INT_MAX},
    {"--output-high-water", &Config::outputHighWater, 1, INT_MAX},
    {"--idle-timeout-ms", &Config::idleTimeoutMs, 1, INT_MAX},
    {"--partial-frame-timeout-ms", &Config::partialFrameTimeoutMs, 1, INT_MAX},
    {"--write-stall-timeout-ms", &Config::writeStallTimeoutMs, 1, INT_MAX},
    {"--linger-timeout-ms", &Config::lingerTimeoutMs, 1, INT_MAX},
    {"--shutdown-timeout-ms", &Config::shutdownTimeoutMs, 1, INT_MAX},
};

bool parsePoller(const std::string& value, PollerKind& poller,
                 std::string& error) {
    if (value == "poll") {
        poller = PollerKind::Poll;
        return true;
    }

    if (value == "epoll") {
#ifdef __linux__
        poller = PollerKind::Epoll;
        return true;
#else
        error = "--poller epoll is only available on Linux";
        return false;
#endif
    }

    error = "invalid value for --poller: " + value;
    return false;
}

} // namespace

bool parseConfig(int argc, const char* const* argv, Config& config,
                 std::string& error) {
    for (int i = 1; i < argc; i += 2) {
        std::string name = argv[i];

        // The one option whose value is a word and not a number.
        if (name == "--poller") {
            if (i + 1 >= argc) {
                error = name + " needs a value";
                return false;
            }

            if (!parsePoller(argv[i + 1], config.poller, error))
                return false;

            continue;
        }

        const Option* option = nullptr;

        for (const Option& candidate : OPTIONS) {
            if (name == candidate.name)
                option = &candidate;
        }

        if (option == nullptr) {
            error = "unknown option: " + name;
            return false;
        }

        if (i + 1 >= argc) {
            error = name + " needs a value";
            return false;
        }

        if (!parseInt(argv[i + 1], option->min, option->max,
                      config.*(option->field))) {
            error = "invalid value for " + name + ": " + argv[i + 1];
            return false;
        }
    }

    return true;
}

const char* pollerName(PollerKind kind) {
    return kind == PollerKind::Epoll ? "epoll" : "poll";
}

std::string optionSummary() {
    Config defaults;
    std::string summary;

    summary += "  --poller poll|epoll   (default ";
    summary += pollerName(defaults.poller);
    summary += ")\n";

    for (const Option& option : OPTIONS) {
        summary += "  ";
        summary += option.name;
        summary += " N   (default ";
        summary += std::to_string(defaults.*(option.field));
        summary += ")\n";
    }

    return summary;
}
