#include "Config.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

bool parse(std::vector<const char*> args, Config& config, std::string& error) {
    args.insert(args.begin(), "server");

    return parseConfig(static_cast<int>(args.size()), args.data(), config,
                       error);
}

bool fails(std::vector<const char*> args) {
    Config config;
    std::string error;

    return !parse(args, config, error) && !error.empty();
}

} // namespace

TEST(Config, DefaultsWhenNoArguments) {
    Config config;
    std::string error;

    ASSERT_TRUE(parse({}, config, error));

    EXPECT_EQ(config.port, 8080);
    EXPECT_EQ(config.workers, 8);
    EXPECT_EQ(config.maxConnections, 10000);
    EXPECT_EQ(config.backlog, SOMAXCONN);
    EXPECT_EQ(config.maxFrame, 1048576);
    EXPECT_EQ(config.outputHighWater, 4194304);
    EXPECT_EQ(config.idleTimeoutMs, 60000);
    EXPECT_EQ(config.partialFrameTimeoutMs, 10000);
    EXPECT_EQ(config.writeStallTimeoutMs, 10000);
    EXPECT_EQ(config.lingerTimeoutMs, 2000);
    EXPECT_EQ(config.shutdownTimeoutMs, 5000);
}

TEST(Config, ReadsEveryOption) {
    Config config;
    std::string error;

    ASSERT_TRUE(parse({"--port",
                       "9000",
                       "--workers",
                       "2",
                       "--max-connections",
                       "50",
                       "--backlog",
                       "64",
                       "--max-frame",
                       "4096",
                       "--output-high-water",
                       "65536",
                       "--idle-timeout-ms",
                       "100",
                       "--partial-frame-timeout-ms",
                       "200",
                       "--write-stall-timeout-ms",
                       "300",
                       "--linger-timeout-ms",
                       "400",
                       "--shutdown-timeout-ms",
                       "500"},
                      config, error));

    EXPECT_EQ(config.port, 9000);
    EXPECT_EQ(config.workers, 2);
    EXPECT_EQ(config.maxConnections, 50);
    EXPECT_EQ(config.backlog, 64);
    EXPECT_EQ(config.maxFrame, 4096);
    EXPECT_EQ(config.outputHighWater, 65536);
    EXPECT_EQ(config.idleTimeoutMs, 100);
    EXPECT_EQ(config.partialFrameTimeoutMs, 200);
    EXPECT_EQ(config.writeStallTimeoutMs, 300);
    EXPECT_EQ(config.lingerTimeoutMs, 400);
    EXPECT_EQ(config.shutdownTimeoutMs, 500);
}

TEST(Config, RejectsUnknownOption) {
    EXPECT_TRUE(fails({"--verbose", "1"}));
    EXPECT_TRUE(fails({"9000"}));
}

TEST(Config, RejectsMissingValue) {
    EXPECT_TRUE(fails({"--port"}));
    EXPECT_TRUE(fails({"--workers", "2", "--port"}));
}

TEST(Config, RejectsValuesThatAreNotWholeNumbers) {
    EXPECT_TRUE(fails({"--port", "abc"}));
    EXPECT_TRUE(fails({"--port", "80x"}));
    EXPECT_TRUE(fails({"--port", ""}));
    EXPECT_TRUE(fails({"--port", "8080.5"}));
    EXPECT_TRUE(fails({"--workers", "99999999999999999999"}));
}

TEST(Config, RejectsValuesOutOfRange) {
    EXPECT_TRUE(fails({"--port", "0"}));
    EXPECT_TRUE(fails({"--port", "65536"}));
    EXPECT_TRUE(fails({"--workers", "0"}));
    EXPECT_TRUE(fails({"--workers", "-1"}));
    EXPECT_TRUE(fails({"--max-connections", "0"}));
    EXPECT_TRUE(fails({"--backlog", "0"}));
    EXPECT_TRUE(fails({"--max-frame", "7"}));
    EXPECT_TRUE(fails({"--output-high-water", "0"}));
    EXPECT_TRUE(fails({"--idle-timeout-ms", "0"}));
    EXPECT_TRUE(fails({"--linger-timeout-ms", "-5"}));
}

TEST(Config, AcceptsRangeLimits) {
    Config config;
    std::string error;

    EXPECT_TRUE(parse({"--port", "1"}, config, error));
    EXPECT_TRUE(parse({"--port", "65535"}, config, error));
    EXPECT_TRUE(parse({"--max-frame", "8"}, config, error));
}

TEST(Config, SummaryListsEveryOptionWithItsDefault) {
    std::string summary = optionSummary();

    EXPECT_NE(summary.find("--port N   (default 8080)"), std::string::npos);
    EXPECT_NE(summary.find("--idle-timeout-ms N   (default 60000)"),
              std::string::npos);
    EXPECT_NE(summary.find("--linger-timeout-ms N   (default 2000)"),
              std::string::npos);
}

TEST(Config, PollerDefaultsToEpollOnLinuxAndPollElsewhere) {
    Config config;

#ifdef __linux__
    EXPECT_EQ(config.poller, PollerKind::Epoll);
#else
    EXPECT_EQ(config.poller, PollerKind::Poll);
#endif
}

TEST(Config, PollerCanBeSetToPoll) {
    Config config;
    std::string error;

    ASSERT_TRUE(parse({"--poller", "poll", "--port", "9000"}, config, error));

    EXPECT_EQ(config.poller, PollerKind::Poll);
    EXPECT_EQ(config.port, 9000);
}

TEST(Config, EpollIsAcceptedOnlyOnLinux) {
    Config config;
    std::string error;

#ifdef __linux__
    ASSERT_TRUE(parse({"--poller", "epoll"}, config, error));
    EXPECT_EQ(config.poller, PollerKind::Epoll);
#else
    EXPECT_FALSE(parse({"--poller", "epoll"}, config, error));
    EXPECT_NE(error.find("only available on Linux"), std::string::npos);
#endif
}

TEST(Config, RejectsUnknownPollersAndAMissingValue) {
    EXPECT_TRUE(fails({"--poller", "kqueue"}));
    EXPECT_TRUE(fails({"--poller", ""}));
    EXPECT_TRUE(fails({"--poller"}));
}

TEST(Config, SummaryListsThePoller) {
    EXPECT_NE(optionSummary().find("--poller poll|epoll"), std::string::npos);
}
