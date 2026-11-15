#include "Buffer.h"

#include <gtest/gtest.h>

#include <string>

TEST(Buffer, StartsEmpty) {
    Buffer buffer;

    EXPECT_EQ(buffer.size(), 0u);
    EXPECT_EQ(buffer.view(), "");
}

TEST(Buffer, AppendAddsAtTheBack) {
    Buffer buffer;

    buffer.append("abc");
    buffer.append("def");

    EXPECT_EQ(buffer.view(), "abcdef");
}

TEST(Buffer, ConsumeRemovesFromTheFront) {
    Buffer buffer;
    buffer.append("abcdefgh");

    buffer.consume(1);
    EXPECT_EQ(buffer.view(), "bcdefgh");

    buffer.consume(5);
    EXPECT_EQ(buffer.view(), "gh");
}

TEST(Buffer, ConsumingEverythingLeavesItEmpty) {
    Buffer buffer;
    buffer.append("abc");

    buffer.consume(3);

    EXPECT_EQ(buffer.size(), 0u);

    buffer.append("xyz");
    EXPECT_EQ(buffer.view(), "xyz");
}

TEST(Buffer, AppendAfterPartialConsumeKeepsOrder) {
    Buffer buffer;
    buffer.append("abcdefgh");

    buffer.consume(2);
    buffer.append("ij");

    EXPECT_EQ(buffer.view(), "cdefghij");
}

TEST(Buffer, ClearDropsEverything) {
    Buffer buffer;
    buffer.append("abcdef");
    buffer.consume(2);

    buffer.clear();

    EXPECT_EQ(buffer.size(), 0u);

    buffer.append("x");
    EXPECT_EQ(buffer.view(), "x");
}

// Many uneven appends and consumes, checked against a plain string. This
// crosses the compaction point over and over.
TEST(Buffer, BehavesLikeAQueueAcrossCompaction) {
    Buffer buffer;
    std::string expected;
    unsigned state = 12345;

    auto next = [&state](unsigned limit) {
        state = state * 1103515245u + 12345u;
        return (state >> 16) % limit;
    };

    for (int round = 0; round < 5000; ++round) {
        std::string chunk(next(40), static_cast<char>('a' + round % 26));
        buffer.append(chunk);
        expected.append(chunk);

        std::size_t count = next(static_cast<unsigned>(expected.size()) + 1);
        buffer.consume(count);
        expected.erase(0, count);

        ASSERT_EQ(buffer.view(), expected) << "round " << round;
    }
}
