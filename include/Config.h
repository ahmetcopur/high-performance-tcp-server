#pragma once

#include <string>

#include <sys/socket.h>

struct Config {
    int port = 8080;
    int workers = 8;
    int backlog = SOMAXCONN;
};

// Reads "--name value" pairs into config. On failure returns false and
// puts a message for the user in error.
bool parseConfig(int argc, const char* const* argv, Config& config,
                 std::string& error);
