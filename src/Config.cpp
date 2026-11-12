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

} // namespace

bool parseConfig(int argc, const char* const* argv, Config& config,
                 std::string& error) {
    for (int i = 1; i < argc; i += 2) {
        std::string name = argv[i];
        int* target = nullptr;
        int max = INT_MAX;

        if (name == "--port") {
            target = &config.port;
            max = 65535;
        }
        else if (name == "--workers") {
            target = &config.workers;
        }
        else if (name == "--backlog") {
            target = &config.backlog;
        }
        else {
            error = "unknown option: " + name;
            return false;
        }

        if (i + 1 >= argc) {
            error = name + " needs a value";
            return false;
        }

        if (!parseInt(argv[i + 1], 1, max, *target)) {
            error = "invalid value for " + name + ": " + argv[i + 1];
            return false;
        }
    }

    return true;
}
