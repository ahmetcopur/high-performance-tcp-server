#pragma once

#include "Poller.h"

#include <cstddef>
#include <unordered_map>
#include <vector>

#include <poll.h>

// Poller on top of poll(). The pollfd array is kept between calls and
// edited in place, not rebuilt for every wait.
class PollPoller : public Poller {
private:
    std::vector<pollfd> fds;

    // Position of each descriptor in fds.
    std::unordered_map<int, std::size_t> indexOf;

public:
    bool add(int fd, bool read, bool write) override;
    void modify(int fd, bool read, bool write) override;
    void remove(int fd) override;
    bool wait(int timeoutMs, std::vector<PollEvent>& events) override;
};
