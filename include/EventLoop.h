#pragma once

#include "FileDescriptor.h"
#include "Poller.h"

#include <chrono>
#include <csignal>
#include <cstdint>
#include <memory>
#include <mutex>
#include <queue>
#include <string>

// What a worker hands back when it has finished a request. This is the
// only way anything travels from a worker thread to the event loop.
struct Completion {
    // The connection the request came from. A descriptor number can be
    // given to a new connection the moment the old one closes, so the id,
    // which is never reused, has to match as well.
    int fd;
    std::uint64_t id;

    // The encoded response.
    std::string response;

    // The request threw an exception instead of producing a response.
    bool failed;
};

// The loop every socket is served from. It owns the poller, the wakeup
// pipe and the completion queue, and decides when to sleep and for how
// long. What to do when something happens is the Handler's business.
class EventLoop {
public:
    using Clock = std::chrono::steady_clock;

    // The four things that can happen. Every call is made on the thread
    // that is inside run().
    class Handler {
    public:
        virtual ~Handler() = default;

        // requestShutdown() was called. Happens once.
        virtual void onShutdownRequested() = 0;

        // A worker has finished a request.
        virtual void onCompletion(Completion& completion) = 0;

        // Called once per iteration, just before the loop sleeps. Deals
        // with whatever has run out of time, and returns how long the
        // loop may sleep in milliseconds, or -1 if only an event should
        // wake it.
        virtual int onTimers(Clock::time_point now) = 0;

        // A watched descriptor is ready.
        virtual void onSocketEvent(const PollEvent& event,
                                   Clock::time_point now) = 0;
    };

private:
    std::unique_ptr<Poller> watcher;

    // A byte written to one end makes the poller report the other end,
    // which is how another thread or a signal handler ends the sleep.
    FileDescriptor wakeRead;
    FileDescriptor wakeWrite;

    std::mutex completionMutex;
    std::queue<Completion> completions;

    volatile std::sig_atomic_t shutdownRequested = 0;
    bool stopped = false;

    void wake();

public:
    // Throws std::system_error if the wakeup pipe cannot be created.
    EventLoop();

    // For registering descriptors. Only for the loop's own thread.
    Poller& poller() {
        return *watcher;
    }

    // Runs until a handler calls stop().
    void run(Handler& handler);

    // Makes run() return once the current step is done. Only for
    // handlers.
    void stop() {
        stopped = true;
    }

    // Hands a finished request to the loop. For worker threads.
    void post(Completion completion);

    // Asks for a shutdown. Safe from any thread and from a signal
    // handler.
    void requestShutdown();
};
