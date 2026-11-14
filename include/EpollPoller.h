#pragma once

// epoll exists only on Linux. Everywhere else this header is empty.
#ifdef __linux__

#include "FileDescriptor.h"
#include "Poller.h"

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include <sys/epoll.h>

// Poller on top of epoll, level-triggered.
//
// poll() is handed the whole list of descriptors on every call and looks
// at each one. epoll keeps the list in the kernel and hands back only
// the descriptors that are ready, so a wait costs the same with ten idle
// connections as with ten thousand.
//
// Level-triggered means a descriptor is reported for as long as it is
// ready, the same as poll(). Nothing is lost if the owner does not take
// everything at once.
class EpollPoller : public Poller {
private:
    FileDescriptor epollFd;

    // What each descriptor is registered for. Changing a registration is
    // a system call, so modify() looks here first and skips the call when
    // nothing would change.
    std::unordered_map<int, std::uint32_t> registered;

    // Where epoll_wait() writes. If more descriptors are ready than fit,
    // the rest are reported by the next wait.
    std::vector<epoll_event> ready;

public:
    // Throws std::system_error if the epoll instance cannot be created.
    explicit EpollPoller(std::size_t maxEventsPerWait = 1024);

    bool add(int fd, bool read, bool write) override;
    void modify(int fd, bool read, bool write) override;
    void remove(int fd) override;
    bool wait(int timeoutMs, std::vector<PollEvent>& events) override;
};

#endif
