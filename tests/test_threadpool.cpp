#include "ThreadPool.h"

#include <gtest/gtest.h>

#include <atomic>
#include <stdexcept>

TEST(ThreadPool, RunsEveryTaskBeforeDestructorReturns) {
    std::atomic<int> count{0};

    {
        ThreadPool pool(4);

        for (int i = 0; i < 1000; ++i)
            pool.enqueue([&count]() { ++count; });
    }

    EXPECT_EQ(count, 1000);
}

TEST(ThreadPool, FutureCarriesResult) {
    ThreadPool pool(2);

    auto result = pool.enqueue([]() { return 42; });

    EXPECT_EQ(result.get(), 42);
}

TEST(ThreadPool, FutureCarriesException) {
    ThreadPool pool(1);

    auto result = pool.enqueue([]() -> int {
        throw std::runtime_error("boom");
    });

    EXPECT_THROW((void)result.get(), std::runtime_error);
}

TEST(ThreadPool, EnqueueAfterShutdownThrows) {
    ThreadPool pool(1);
    pool.shutdown();

    EXPECT_THROW(pool.enqueue([]() {}), std::runtime_error);
}

TEST(ThreadPool, ZeroThreadsIsRejected) {
    EXPECT_THROW(ThreadPool pool(0), std::invalid_argument);
}
