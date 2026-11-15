#include "ThreadPool.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>

ThreadPool::ThreadPool(std::size_t workerCount, std::size_t queueCapacity)
    : capacity(queueCapacity) {
    if (workerCount == 0)
        throw std::invalid_argument("Thread count must be positive");

    if (queueCapacity == 0)
        throw std::invalid_argument("Queue capacity must be positive");

    try {
        for (std::size_t i = 0; i < workerCount; ++i)
            workers.emplace_back([this]() {
                work();
            });
    } catch (...) {
        shutdown();
        throw;
    }
}

ThreadPool::~ThreadPool() {
    shutdown();
}

void ThreadPool::work() {
    while (true) {
        std::unique_ptr<Task> task;

        {
            std::unique_lock<std::mutex> lock(queueMutex);

            ++sleeping;
            available.wait(lock, [this]() {
                return stopping || !tasks.empty();
            });
            --sleeping;

            // Only reached empty when stopping. What was queued before
            // the stop is still run.
            if (tasks.empty())
                return;

            task = std::move(tasks.front());
            tasks.pop_front();
        }

        auto started = std::chrono::steady_clock::now();

        // A task that throws must not take its worker thread with it.
        try {
            task->run();
        } catch (...) {
            failed.fetch_add(1, std::memory_order_relaxed);
        }

        // Whatever the task captured is released before it is counted
        // as done.
        task.reset();

        auto elapsed = std::chrono::steady_clock::now() - started;

        busyNanoseconds.fetch_add(
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed)
                    .count()),
            std::memory_order_relaxed);

        completed.fetch_add(1, std::memory_order_relaxed);
    }
}

void ThreadPool::wakeWorkers() {
    std::size_t wanted;
    bool all;

    {
        std::lock_guard<std::mutex> lock(queueMutex);

        wanted = std::min(tasks.size(), sleeping);
        all = wanted == sleeping;
    }

    // A worker counted as sleeping may have been woken already. Waking
    // one too many costs a little and loses nothing.
    if (all) {
        available.notify_all();
        return;
    }

    for (std::size_t i = 0; i < wanted; ++i)
        available.notify_one();
}

void ThreadPool::shutdown() {
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        stopping = true;
    }

    available.notify_all();

    for (std::thread& worker : workers) {
        if (worker.joinable())
            worker.join();
    }
}

ThreadPool::Stats ThreadPool::stats() const {
    Stats snapshot;

    snapshot.workers = workers.size();
    snapshot.capacity = capacity;

    {
        std::lock_guard<std::mutex> lock(queueMutex);

        snapshot.queueDepth = tasks.size();
        snapshot.submitted = submitted;
        snapshot.rejected = rejected;
    }

    snapshot.completed = completed.load(std::memory_order_relaxed);
    snapshot.failed = failed.load(std::memory_order_relaxed);
    snapshot.busyMicroseconds =
        busyNanoseconds.load(std::memory_order_relaxed) / 1000;

    return snapshot;
}
