#pragma once

#include <vector>

// What happened on one descriptor.
struct PollEvent {
    int fd = -1;
    bool readable = false;
    bool writable = false;

    // The peer will send no more. What this implies differs by platform,
    // see docs/architecture.md.
    bool hangup = false;

    // The socket has an error, or the descriptor is not open.
    bool error = false;
};

// Waits for descriptors to become ready. The event loop is written
// against this interface, so that the system call behind it can be
// replaced: poll() today, epoll on Linux later.
class Poller {
public:
    virtual ~Poller() = default;

    // Starts watching a descriptor for the given kinds of readiness.
    virtual void add(int fd, bool read, bool write) = 0;

    // Changes what a watched descriptor is watched for. Calling it with
    // what is already set is allowed and cheap.
    virtual void modify(int fd, bool read, bool write) = 0;

    // Stops watching a descriptor. Call it before closing the descriptor.
    virtual void remove(int fd) = 0;

    // Blocks until something is ready or timeoutMs has passed. -1 means
    // no timeout. Puts what happened into events, replacing its contents.
    // Returns false if waiting itself failed, with errno set. Being
    // interrupted by a signal is not a failure: it returns true with no
    // events.
    virtual bool wait(int timeoutMs, std::vector<PollEvent>& events) = 0;
};
