#include "Buffer.h"

#include <gtest/gtest.h>

#include <cstring>
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

TEST(Buffer, BytesWrittenInPlaceAreHeldLikeAppendedOnes) {
    Buffer buffer;
    buffer.append("ab");

    char* room = buffer.writable(8);
    std::memcpy(room, "cdef", 4);
    buffer.written(4);

    EXPECT_EQ(buffer.view(), "abcdef");

    buffer.append("g");
    EXPECT_EQ(buffer.view(), "abcdefg");
}

TEST(Buffer, AskingForRoomAndUsingNoneChangesNothing) {
    Buffer buffer;
    buffer.append("abc");
    buffer.consume(1);

    buffer.writable(100000);
    buffer.written(0);

    EXPECT_EQ(buffer.view(), "bc");
}

// The same as the queue test above, with every second piece written in
// place. Asking for room moves or replaces the block, which must not
// disturb what is held.
TEST(Buffer, BehavesLikeAQueueWhenWrittenInPlace) {
    Buffer buffer;
    std::string expected;
    unsigned state = 54321;

    auto next = [&state](unsigned limit) {
        state = state * 1103515245u + 12345u;
        return (state >> 16) % limit;
    };

    for (int round = 0; round < 5000; ++round) {
        std::string chunk(next(300), static_cast<char>('a' + round % 26));

        if (round % 2 == 0) {
            buffer.append(chunk);
        }
        else {
            // Ask for more than is used, as a read does.
            char* room = buffer.writable(chunk.size() + next(200));

            if (!chunk.empty())
                std::memcpy(room, chunk.data(), chunk.size());

            buffer.written(chunk.size());
        }

        expected.append(chunk);

        std::size_t count = next(static_cast<unsigned>(expected.size()) + 1);
        buffer.consume(count);
        expected.erase(0, count);

        ASSERT_EQ(buffer.view(), expected) << "round " << round;
    }
}
