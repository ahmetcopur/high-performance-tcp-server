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
    EXPECT_EQ(config.backlog, SOMAXCONN);
}

TEST(Config, ReadsEveryOption) {
    Config config;
    std::string error;

    ASSERT_TRUE(parse({"--port", "9000", "--workers", "2", "--backlog", "64"},
                      config, error));

    EXPECT_EQ(config.port, 9000);
    EXPECT_EQ(config.workers, 2);
    EXPECT_EQ(config.backlog, 64);
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
    EXPECT_TRUE(fails({"--backlog", "0"}));
}

TEST(Config, AcceptsRangeLimits) {
    Config config;
    std::string error;

    EXPECT_TRUE(parse({"--port", "1"}, config, error));
    EXPECT_TRUE(parse({"--port", "65535"}, config, error));
}
