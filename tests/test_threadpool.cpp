#include "ThreadPool.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

// Holds workers at a known point until the test lets them go.
class Gate {
private:
    std::mutex mutex;
    std::condition_variable changed;
    bool open = false;
    int waiting = 0;

public:
    // Called by a task: blocks until release().
    void wait() {
        std::unique_lock<std::mutex> lock(mutex);

        ++waiting;
        changed.notify_all();

        changed.wait(lock, [this]() {
            return open;
        });
    }

    // Called by the test: returns once that many tasks are blocked.
    void waitUntilBlocked(int count) {
        std::unique_lock<std::mutex> lock(mutex);

        changed.wait(lock, [this, count]() {
            return waiting >= count;
        });
    }

    void release() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            open = true;
        }

        changed.notify_all();
    }
};

} // namespace

TEST(ThreadPool, RunsEveryTaskBeforeDestructorReturns) {
    std::atomic<int> count{0};

    {
        ThreadPool pool(4, 1000);

        for (int i = 0; i < 1000; ++i)
            ASSERT_TRUE(pool.trySubmit([&count]() {
                ++count;
            }));
    }

    EXPECT_EQ(count, 1000);
}

TEST(ThreadPool, RunsATaskThatCanOnlyBeMoved) {
    std::atomic<int> seen{0};

    {
        ThreadPool pool(1, 8);
        auto value = std::make_unique<int>(42);

        ASSERT_TRUE(pool.trySubmit([&seen, value = std::move(value)]() {
            seen = *value;
        }));
    }

    EXPECT_EQ(seen, 42);
}

TEST(ThreadPool, RefusesTasksWhenTheQueueIsFull) {
    Gate gate;
    std::atomic<int> ran{0};
    ThreadPool pool(1, 2);

    // The only worker takes this one and stays in it.
    ASSERT_TRUE(pool.trySubmit([&gate]() {
        gate.wait();
    }));
    gate.waitUntilBlocked(1);

    auto count = [&ran]() {
        ++ran;
    };

    // Two fit in the queue. The task being run does not count.
    EXPECT_TRUE(pool.trySubmit(count));
    EXPECT_TRUE(pool.trySubmit(count));
    EXPECT_FALSE(pool.trySubmit(count));
    EXPECT_FALSE(pool.trySubmit(count));

    ThreadPool::Stats full = pool.stats();
    EXPECT_EQ(full.queueDepth, 2u);
    EXPECT_EQ(full.submitted, 3u);
    EXPECT_EQ(full.rejected, 2u);

    gate.release();
    pool.shutdown();

    EXPECT_EQ(ran, 2);
}

TEST(ThreadPool, HasRoomAgainOnceAWorkerTakesATask) {
    Gate first;
    Gate second;
    ThreadPool pool(1, 1);

    ASSERT_TRUE(pool.trySubmit([&first]() {
        first.wait();
    }));
    first.waitUntilBlocked(1);

    ASSERT_TRUE(pool.trySubmit([&second]() {
        second.wait();
    }));
    ASSERT_FALSE(pool.trySubmit([]() {}));

    // The worker moves on to the queued task, which empties the queue.
    first.release();
    second.waitUntilBlocked(1);

    EXPECT_TRUE(pool.trySubmit([]() {}));

    second.release();
}

// A refused task is handed back untouched, so the caller still has
// whatever it captured.
TEST(ThreadPool, ARefusedTaskIsLeftAsItWas) {
    Gate gate;
    std::atomic<int> seen{0};
    ThreadPool pool(1, 1);

    ASSERT_TRUE(pool.trySubmit([&gate]() {
        gate.wait();
    }));
    gate.waitUntilBlocked(1);
    ASSERT_TRUE(pool.trySubmit([]() {}));

    auto task = [&seen, value = std::make_unique<int>(7)]() {
        seen = *value;
    };

    ASSERT_FALSE(pool.trySubmit(std::move(task)));

    gate.release();

    // Room appears once the worker has taken the queued task.
    while (!pool.trySubmit(std::move(task)))
        std::this_thread::yield();

    pool.shutdown();

    EXPECT_EQ(seen, 7);
}

TEST(ThreadPool, OneWorkerRunsTasksInTheOrderTheyWereSubmitted) {
    std::vector<int> order;

    {
        ThreadPool pool(1, 1000);

        for (int i = 0; i < 1000; ++i)
            ASSERT_TRUE(pool.trySubmit([&order, i]() {
                order.push_back(i);
            }));
    }

    ASSERT_EQ(order.size(), 1000u);

    for (int i = 0; i < 1000; ++i)
        ASSERT_EQ(order[static_cast<std::size_t>(i)], i);
}

// Several threads submit at once. With one worker, each producer's own
// tasks must still run in the order that producer submitted them.
TEST(ThreadPool, KeepsEachProducersOrder) {
    const int producers = 4;
    const int perProducer = 500;
    std::vector<std::vector<int>> seen(producers);

    {
        ThreadPool pool(1, 64);
        std::vector<std::thread> threads;

        for (int p = 0; p < producers; ++p)
            threads.emplace_back([&pool, &seen, p]() {
                for (int i = 0; i < perProducer; ++i) {
                    while (!pool.trySubmit([&seen, p, i]() {
                        seen[static_cast<std::size_t>(p)].push_back(i);
                    }))
                        std::this_thread::yield();
                }
            });

        for (std::thread& thread : threads)
            thread.join();
    }

    for (const std::vector<int>& sequence : seen) {
        ASSERT_EQ(sequence.size(), static_cast<std::size_t>(perProducer));

        for (int i = 0; i < perProducer; ++i)
            ASSERT_EQ(sequence[static_cast<std::size_t>(i)], i);
    }
}

TEST(ThreadPool, ATaskThatThrowsDoesNotStopItsWorker) {
    std::atomic<int> ran{0};
    ThreadPool pool(1, 8);

    ASSERT_TRUE(pool.trySubmit([]() {
        throw std::runtime_error("boom");
    }));
    ASSERT_TRUE(pool.trySubmit([]() {
        throw 42;
    }));
    ASSERT_TRUE(pool.trySubmit([&ran]() {
        ++ran;
    }));

    pool.shutdown();

    EXPECT_EQ(ran, 1);

    ThreadPool::Stats stats = pool.stats();
    EXPECT_EQ(stats.completed, 3u);
    EXPECT_EQ(stats.failed, 2u);
}

TEST(ThreadPool, ShutdownRunsWhatIsAlreadyQueued) {
    Gate gate;
    std::atomic<int> ran{0};
    ThreadPool pool(1, 100);

    ASSERT_TRUE(pool.trySubmit([&gate]() {
        gate.wait();
    }));
    gate.waitUntilBlocked(1);

    for (int i = 0; i < 100; ++i)
        ASSERT_TRUE(pool.trySubmit([&ran]() {
            ++ran;
        }));

    // Shutdown waits for the workers, so the gate has to open from
    // another thread while it waits.
    std::thread opener([&gate]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        gate.release();
    });

    pool.shutdown();
    opener.join();

    EXPECT_EQ(ran, 100);
}

TEST(ThreadPool, RefusesTasksAfterShutdown) {
    std::atomic<int> ran{0};
    ThreadPool pool(2, 8);

    pool.shutdown();

    EXPECT_FALSE(pool.trySubmit([&ran]() {
        ++ran;
    }));
    EXPECT_EQ(ran, 0);
    EXPECT_EQ(pool.stats().rejected, 1u);
}

TEST(ThreadPool, ShutdownCanBeCalledTwice) {
    ThreadPool pool(2, 8);

    pool.shutdown();
    pool.shutdown();
}

TEST(ThreadPool, RejectsZeroWorkersAndZeroCapacity) {
    EXPECT_THROW(ThreadPool pool(0, 8), std::invalid_argument);
    EXPECT_THROW(ThreadPool pool(2, 0), std::invalid_argument);
}

TEST(ThreadPool, CountsWhatItDid) {
    ThreadPool pool(3, 50);

    ThreadPool::Stats fresh = pool.stats();
    EXPECT_EQ(fresh.workers, 3u);
    EXPECT_EQ(fresh.capacity, 50u);
    EXPECT_EQ(fresh.queueDepth, 0u);
    EXPECT_EQ(fresh.submitted, 0u);
    EXPECT_EQ(fresh.rejected, 0u);
    EXPECT_EQ(fresh.completed, 0u);
    EXPECT_EQ(fresh.failed, 0u);
    EXPECT_EQ(fresh.busyMicroseconds, 0u);

    for (int i = 0; i < 20; ++i)
        while (!pool.trySubmit([]() {}))
            std::this_thread::yield();

    pool.shutdown();

    ThreadPool::Stats done = pool.stats();
    EXPECT_EQ(done.queueDepth, 0u);
    EXPECT_EQ(done.submitted, 20u);
    EXPECT_EQ(done.completed, 20u);
    EXPECT_EQ(done.failed, 0u);
}

TEST(ThreadPool, MeasuresTheTimeWorkersSpendOnTasks) {
    ThreadPool pool(2, 8);

    for (int i = 0; i < 2; ++i)
        ASSERT_TRUE(pool.trySubmit([]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }));

    pool.shutdown();

    // Two tasks of 50 ms each, added up over both workers.
    EXPECT_GE(pool.stats().busyMicroseconds, 90000u);
    EXPECT_LT(pool.stats().busyMicroseconds, 5000000u);
}

// Many threads submitting into a small queue while several workers
// drain it. Every task must run exactly once. Run under ThreadSanitizer
// this is the test that would show a race in the queue.
TEST(ThreadPool, ManyProducersAndWorkersLoseNothing) {
    const int producers = 8;
    const int perProducer = 5000;
    std::vector<std::atomic<int>> runs(producers * perProducer);
    std::atomic<std::uint64_t> refused{0};

    for (std::atomic<int>& count : runs)
        count = 0;

    ThreadPool pool(4, 16);
    std::vector<std::thread> threads;

    for (int p = 0; p < producers; ++p)
        threads.emplace_back([&pool, &runs, &refused, p]() {
            for (int i = 0; i < perProducer; ++i) {
                std::size_t index =
                    static_cast<std::size_t>(p * perProducer + i);

                while (!pool.trySubmit([&runs, index]() {
                    ++runs[index];
                })) {
                    ++refused;
                    std::this_thread::yield();
                }
            }
        });

    for (std::thread& thread : threads)
        thread.join();

    pool.shutdown();

    for (const std::atomic<int>& count : runs)
        ASSERT_EQ(count, 1);

    ThreadPool::Stats stats = pool.stats();
    EXPECT_EQ(stats.submitted,
              static_cast<std::uint64_t>(producers * perProducer));
    EXPECT_EQ(stats.completed, stats.submitted);
    EXPECT_EQ(stats.rejected, refused.load());
    EXPECT_EQ(stats.queueDepth, 0u);
}

namespace {

// Waits until the pool has run that many tasks. False if it does not
// get there in five seconds, which means tasks were left lying.
bool completes(const ThreadPool& pool, std::uint64_t count) {
    auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);

    while (pool.stats().completed < count) {
        if (std::chrono::steady_clock::now() > deadline)
            return false;

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    return true;
}

} // namespace

TEST(ThreadPool, OneWakeupIsEnoughForManyQueuedTasks) {
    std::atomic<int> count{0};
    ThreadPool pool(2, 64);

    // Let the workers fall asleep first.
    ASSERT_TRUE(pool.trySubmit([]() {
    }));
    ASSERT_TRUE(completes(pool, 1));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    for (int i = 0; i < 50; ++i)
        ASSERT_TRUE(pool.tryQueue([&count]() {
            ++count;
        }));

    pool.wakeWorkers();

    // No shutdown here to sweep up: the wakeup alone must do it.
    EXPECT_TRUE(completes(pool, 51));
    EXPECT_EQ(count, 50);
}

TEST(ThreadPool, WakingWithNothingQueuedDoesNothing) {
    ThreadPool pool(2, 4);

    pool.wakeWorkers();
    pool.wakeWorkers();

    EXPECT_EQ(pool.stats().completed, 0u);
}

TEST(ThreadPool, QueueingQuietlyIsRefusedAndCountedLikeSubmitting) {
    Gate gate;
    ThreadPool pool(1, 1);

    ASSERT_TRUE(pool.trySubmit([&gate]() {
        gate.wait();
    }));
    gate.waitUntilBlocked(1);

    EXPECT_TRUE(pool.tryQueue([]() {
    }));
    EXPECT_FALSE(pool.tryQueue([]() {
    }));

    ThreadPool::Stats stats = pool.stats();
    EXPECT_EQ(stats.submitted, 2u);
    EXPECT_EQ(stats.rejected, 1u);
    EXPECT_EQ(stats.queueDepth, 1u);

    gate.release();
}

TEST(ThreadPool, QueueingInBatchesWithOneWakeupEachLosesNothing) {
    const int producers = 4;
    const int batches = 500;
    const int perBatch = 8;
    const int total = producers * batches * perBatch;
    std::vector<std::atomic<int>> runs(static_cast<std::size_t>(total));

    for (std::atomic<int>& count : runs)
        count = 0;

    ThreadPool pool(4, 64);
    std::vector<std::thread> threads;

    for (int p = 0; p < producers; ++p)
        threads.emplace_back([&pool, &runs, p]() {
            for (int b = 0; b < batches; ++b) {
                for (int i = 0; i < perBatch; ++i) {
                    std::size_t index = static_cast<std::size_t>(
                        (p * batches + b) * perBatch + i);

                    // A full queue: wake the workers and try again.
                    while (!pool.tryQueue([&runs, index]() {
                        ++runs[index];
                    })) {
                        pool.wakeWorkers();
                        std::this_thread::yield();
                    }
                }

                pool.wakeWorkers();
            }
        });

    for (std::thread& thread : threads)
        thread.join();

    // Before shutdown, which would wake everyone and hide a wakeup that
    // was lost.
    ASSERT_TRUE(completes(pool, static_cast<std::uint64_t>(total)));

    for (const std::atomic<int>& count : runs)
        ASSERT_EQ(count, 1);
}
