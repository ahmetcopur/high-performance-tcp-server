
#pragma once

#include <thread>
#include <vector>
#include <queue>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <future>
#include <memory>
#include <type_traits>
#include <stdexcept>
#include <utility>

class ThreadPool {
private:
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;

    std::mutex queueMutex;
    std::condition_variable condition;
    bool stop = false;

public:
    explicit ThreadPool(size_t numThreads) {
        if (numThreads == 0)
            throw std::invalid_argument("Thread count must be positive");

        try {
            for (size_t i = 0; i < numThreads; ++i) {
                workers.emplace_back([this]() {
                    while (true) {
                        std::function<void()> task;

                        {
                            std::unique_lock<std::mutex> lock(queueMutex);

                            condition.wait(lock, [this]() {
                                return stop || !tasks.empty();
                            });

                            if (stop && tasks.empty())
                                return;

                            task = std::move(tasks.front());
                            tasks.pop();
                        }

                        task();
                    }
                });
            }
        } catch (...) {
            shutdown();
            throw;
        }
    }

    template<typename F>
    auto enqueue(F&& f) -> std::future<std::invoke_result_t<F>> {
        using ReturnType = std::invoke_result_t<F>;

        auto task = std::make_shared<std::packaged_task<ReturnType()>>(
            std::forward<F>(f));

        std::future<ReturnType> result = task->get_future();

        {
            std::lock_guard<std::mutex> lock(queueMutex);

            if (stop)
                throw std::runtime_error("ThreadPool stopped");

            tasks.emplace([task]() {
                (*task)();
            });
        }

        condition.notify_one();
        return result;
    }

    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            stop = true;
        }

        condition.notify_all();

        for (auto& worker : workers) {
            if (worker.joinable())
                worker.join();
        }
    }

    ~ThreadPool() {
        shutdown();
    }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
};
