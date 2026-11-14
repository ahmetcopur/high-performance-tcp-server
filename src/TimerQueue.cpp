#include "TimerQueue.h"

void TimerQueue::arm(int fd, std::uint64_t id, Clock::time_point when) {
    auto it = armed.find(id);

    if (it != armed.end() && it->second <= when)
        return;

    armed[id] = when;
    heap.push({when, fd, id});
}

void TimerQueue::cancel(std::uint64_t id) {
    armed.erase(id);
}

void TimerQueue::dropStale() {
    while (!heap.empty()) {
        const Entry& top = heap.top();
        auto it = armed.find(top.id);

        if (it != armed.end() && it->second == top.when)
            return;

        heap.pop();
    }
}

bool TimerQueue::popDue(Clock::time_point now, Entry& entry) {
    dropStale();

    if (heap.empty() || heap.top().when > now)
        return false;

    entry = heap.top();
    heap.pop();
    armed.erase(entry.id);

    return true;
}

long long TimerQueue::msUntilNext(Clock::time_point now) {
    dropStale();

    if (heap.empty())
        return -1;

    auto left = heap.top().when - now;

    if (left <= Clock::duration::zero())
        return 0;

    // One more than the truncated remainder, so that whoever sleeps this
    // long wakes after the deadline and not just before it.
    return std::chrono::duration_cast<std::chrono::milliseconds>(left).count() +
           1;
}
