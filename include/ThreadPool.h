#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

// A fixed number of worker threads taking tasks from one queue.
//
// The queue has a capacity. When it is full, trySubmit() says so and the
// caller decides what to do, so the pool can never be made to hold an
// unlimited amount of waiting work.
class ThreadPool {
public:
    struct Stats {
        std::size_t workers;
        std::size_t capacity;

        // Tasks waiting in the queue right now. Tasks being run are not
        // counted.
        std::size_t queueDepth;

        std::uint64_t submitted; // accepted by trySubmit()
        std::uint64_t rejected;  // refused by trySubmit()
        std::uint64_t completed; // run to the end, or until they threw
        std::uint64_t failed;    // of those, the ones that threw

        // Time all workers together have spent running tasks.
        std::uint64_t busyMicroseconds;
    };

private:
    // A task is any callable, including one that can only be moved. It
    // is held through this interface so the queue can store tasks of
    // different types.
    struct Task {
        virtual ~Task() = default;
        virtual void run() = 0;
    };

    template<typename F> struct TaskOf : Task {
        F function;

        explicit TaskOf(F&& f) : function(std::move(f)) {
        }

        explicit TaskOf(const F& f) : function(f) {
        }

        void run() override {
            function();
        }
    };

    std::vector<std::thread> workers;
    std::deque<std::unique_ptr<Task>> tasks;
    std::size_t capacity;

    mutable std::mutex queueMutex;
    std::condition_variable available;
    bool stopping = false;

    // Changed only with queueMutex held.
    std::uint64_t submitted = 0;
    std::uint64_t rejected = 0;

    // Changed by workers without the mutex.
    std::atomic<std::uint64_t> completed{0};
    std::atomic<std::uint64_t> failed{0};
    std::atomic<std::uint64_t> busyNanoseconds{0};

    void work();

public:
    // queueCapacity is how many tasks may wait. Tasks being run do not
    // count against it. Throws std::invalid_argument if either is zero.
    ThreadPool(std::size_t workerCount, std::size_t queueCapacity);

    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // Hands a task to the workers. Returns false if the queue is full or
    // the pool is shutting down. In that case the task is left exactly
    // as it was: nothing has been moved out of it.
    template<typename F> bool trySubmit(F&& function) {
        {
            std::lock_guard<std::mutex> lock(queueMutex);

            if (stopping || tasks.size() >= capacity) {
                ++rejected;
                return false;
            }

            tasks.push_back(std::make_unique<TaskOf<std::decay_t<F>>>(
                std::forward<F>(function)));
            ++submitted;
        }

        available.notify_one();
        return true;
    }

    // Lets the workers finish every task already in the queue, then
    // stops them. Returns when all worker threads have ended. Tasks
    // submitted from now on are refused.
    void shutdown();

    Stats stats() const;
};
