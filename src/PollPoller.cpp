#include "PollPoller.h"

#include <cerrno>

namespace {

short interest(bool read, bool write) {
    int events = 0;

    if (read)
        events |= POLLIN;

    if (write)
        events |= POLLOUT;

    return static_cast<short>(events);
}

} // namespace

bool PollPoller::add(int fd, bool read, bool write) {
    indexOf[fd] = fds.size();
    fds.push_back({fd, interest(read, write), 0});

    return true;
}

void PollPoller::modify(int fd, bool read, bool write) {
    fds[indexOf.at(fd)].events = interest(read, write);
}

void PollPoller::remove(int fd) {
    std::size_t index = indexOf.at(fd);

    // The last entry takes the freed place, so the array stays dense
    // without shifting everything behind the removed one.
    fds[index] = fds.back();
    indexOf[fds[index].fd] = index;

    fds.pop_back();
    indexOf.erase(fd);
}

bool PollPoller::wait(int timeoutMs, std::vector<PollEvent>& events) {
    events.clear();

    int ready = poll(fds.data(), static_cast<nfds_t>(fds.size()), timeoutMs);

    if (ready < 0)
        return errno == EINTR;

    for (const pollfd& entry : fds) {
        if (entry.revents == 0)
            continue;

        PollEvent event;
        event.fd = entry.fd;
        event.readable = (entry.revents & POLLIN) != 0;
        event.writable = (entry.revents & POLLOUT) != 0;
        event.hangup = (entry.revents & POLLHUP) != 0;
        event.error = (entry.revents & (POLLERR | POLLNVAL)) != 0;

        events.push_back(event);
    }

    return true;
}
