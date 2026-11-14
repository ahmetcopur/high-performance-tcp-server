#include "Config.h"
#include "TcpServer.h"

#include <csignal>
#include <iostream>
#include <memory>
#include <string>
#include <system_error>

namespace {

// What the signal handler talks to. Set once, before the handler is
// installed.
TcpServer* runningServer = nullptr;

// The first SIGINT or SIGTERM asks the server to shut down. Both signals
// then get their default action back, so a second one ends the process
// at once.
void onSignal(int) {
    runningServer->requestShutdown();

    std::signal(SIGINT, SIG_DFL);
    std::signal(SIGTERM, SIG_DFL);
}

} // namespace

int main(int argc, char** argv) {
    Config config;
    std::string configError;

    if (!parseConfig(argc, argv, config, configError)) {
        std::cerr << configError << "\n"
                  << "Usage: " << argv[0] << " [option value]...\n"
                  << optionSummary();
        return 1;
    }

    // Writing to a connection the peer has closed must fail with an
    // error, not end the process.
    std::signal(SIGPIPE, SIG_IGN);

    std::unique_ptr<TcpServer> server;

    try {
        server = std::make_unique<TcpServer>(config);
    } catch (const std::system_error& e) {
        std::cerr << "cannot start: " << e.what() << '\n';
        return 1;
    }

    runningServer = server.get();

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    std::cout << "Listening on 127.0.0.1:" << config.port << " with "
              << config.workers << " workers (Ctrl+C to stop)" << std::endl;

    server->run();

    std::cout << "Server stopped\n";
    return 0;
}
