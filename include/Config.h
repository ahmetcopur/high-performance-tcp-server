#pragma once

#include <string>

#include <sys/socket.h>

struct Config {
    int port = 8080;
    int workers = 8;
    int maxConnections = 10000;
    int backlog = SOMAXCONN;

    // Largest value a frame's length field may have.
    int maxFrame = 1 << 20;

    // Pending output per connection above which the server stops reading
    // from it. Reading resumes at half of this.
    int outputHighWater = 4 << 20;
};

// Reads "--name value" pairs into config. On failure returns false and
// puts a message for the user in error.
bool parseConfig(int argc, const char* const* argv, Config& config,
                 std::string& error);
