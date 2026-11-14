#include "EventLoop.h"

#include "PollPoller.h"

#include <cerrno>
#include <cstdio>
#include <system_error>
#include <utility>
#include <vector>

#include <unistd.h>

EventLoop::EventLoop() : watcher(std::make_unique<PollPoller>()) {
    int ends[2];

    if (pipe(ends) < 0)
        throw std::system_error(errno, std::generic_category(), "pipe");

    wakeRead.reset(ends[0]);
    wakeWrite.reset(ends[1]);

    // Non-blocking on both ends: a full pipe must not block the thread
    // that wakes the loop, and emptying it must not block the loop.
    if (!wakeRead.setNonBlocking() || !wakeWrite.setNonBlocking())
        throw std::system_error(errno, std::generic_category(), "pipe");

    watcher->add(wakeRead.get(), true, false);
}

// If the pipe is full the write fails with EAGAIN, which is fine because
// a wakeup is already pending. errno is put back because this also runs
// inside a signal handler, on top of whatever the thread was doing.
void EventLoop::wake() {
    int saved = errno;
    char byte = 1;
    ssize_t n = write(wakeWrite.get(), &byte, 1);
    (void)n;
    errno = saved;
}

void EventLoop::post(Completion completion) {
    bool wasEmpty;

    {
        std::lock_guard<std::mutex> lock(completionMutex);

        wasEmpty = completions.empty();
        completions.push(std::move(completion));
    }

    // A non-empty queue already has a wakeup on its way.
    if (wasEmpty)
        wake();
}

void EventLoop::requestShutdown() {
    shutdownRequested = 1;
    wake();
}

void EventLoop::run(Handler& handler) {
    std::vector<PollEvent> events;
    bool shutdownAnnounced = false;

    while (!stopped) {
        if (shutdownRequested && !shutdownAnnounced) {
            shutdownAnnounced = true;
            handler.onShutdownRequested();
        }

        // The queue is swapped out so the lock is not held while the
        // completions are handled, which can submit new work.
        std::queue<Completion> done;

        {
            std::lock_guard<std::mutex> lock(completionMutex);
            std::swap(done, completions);
        }

        while (!done.empty()) {
            handler.onCompletion(done.front());
            done.pop();
        }

        int timeoutMs = handler.onTimers(Clock::now());

        if (stopped)
            break;

        if (!watcher->wait(timeoutMs, events)) {
            perror("poll");
            break;
        }

        Clock::time_point now = Clock::now();

        for (const PollEvent& event : events) {
            if (event.fd != wakeRead.get()) {
                handler.onSocketEvent(event, now);
                continue;
            }

            // Emptied here, before the completion queue is looked at
            // again at the top of the loop. The other way round, a
            // completion could arrive after the look and have its
            // wakeup thrown away.
            char drain[256];

            while (read(wakeRead.get(), drain, sizeof(drain)) > 0) {
            }
        }
    }
}
