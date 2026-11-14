#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <queue>
#include <unordered_map>
#include <vector>

// Remembers when each connection should next be looked at, and hands
// back the ones whose time has come, earliest first.
//
// It is a min-heap with lazy deletion. Changing or cancelling a time
// does not search the heap. The entry that is now out of date stays
// where it is and is thrown away when it reaches the top. What makes an
// entry current is that it agrees with `armed`.
//
// The promise is "no later than". If a connection's deadline moves
// further away, which is the common case, nothing is pushed. Its entry
// comes up at the old time, the owner sees that the connection is not
// due yet, and arms it again. That keeps the heap at about one entry
// per connection however busy the connections are.
class TimerQueue {
public:
    using Clock = std::chrono::steady_clock;

    struct Entry {
        Clock::time_point when;

        // The connection, named the same way a completion names it.
        int fd;
        std::uint64_t id;
    };

private:
    struct Later {
        bool operator()(const Entry& a, const Entry& b) const {
            return a.when > b.when;
        }
    };

    std::priority_queue<Entry, std::vector<Entry>, Later> heap;

    // The time each id is currently set to come up at.
    std::unordered_map<std::uint64_t, Clock::time_point> armed;

    // Pops entries off the top that no longer agree with `armed`.
    void dropStale();

public:
    // Makes sure the id comes up at `when` or sooner.
    void arm(int fd, std::uint64_t id, Clock::time_point when);

    // The id will not come up. For a connection that has been closed.
    void cancel(std::uint64_t id);

    // Takes the earliest entry if its time has come. Returns false if
    // nothing is due. An id that has come up is no longer armed.
    bool popDue(Clock::time_point now, Entry& entry);

    // Milliseconds until the earliest entry is due, rounded up. 0 if one
    // is due already, -1 if nothing is armed.
    long long msUntilNext(Clock::time_point now);

    // Entries in the heap, out-of-date ones included.
    std::size_t heapSize() const {
        return heap.size();
    }
};
