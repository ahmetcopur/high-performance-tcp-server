#include "EventLoop.h"

#include "PollPoller.h"

#include <cstdio>
#include <utility>
#include <vector>

EventLoop::EventLoop() : watcher(std::make_unique<PollPoller>()) {
    watcher->add(waker.fd(), true, false);
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
        waker.notify();
}

void EventLoop::requestShutdown() {
    shutdownRequested = 1;
    waker.notify();
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

        Clock::time_point now = Clock::now();

        while (!done.empty()) {
            handler.onCompletion(done.front(), now);
            done.pop();
        }

        int timeoutMs = handler.onTimers(now);

        if (stopped)
            break;

        if (!watcher->wait(timeoutMs, events)) {
            perror("poll");
            break;
        }

        now = Clock::now();

        for (const PollEvent& event : events) {
            if (event.fd != waker.fd()) {
                handler.onSocketEvent(event, now);
                continue;
            }

            // Emptied here, before the completion queue is looked at
            // again at the top of the loop. The other way round, a
            // completion could arrive after the look and have its
            // wakeup thrown away.
            waker.drain();
        }
    }
}
