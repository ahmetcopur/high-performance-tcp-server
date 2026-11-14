#include "EpollPoller.h"

#ifdef __linux__

#include <cerrno>
#include <system_error>

namespace {

// EPOLLRDHUP reports that the peer has finished sending. It is asked for
// only together with input. A connection that has stopped reading would
// otherwise be woken by it again and again while it still has responses
// to send, with nothing it could do about it.
std::uint32_t interest(bool read, bool write) {
    std::uint32_t events = 0;

    if (read)
        events |= EPOLLIN | EPOLLRDHUP;

    if (write)
        events |= EPOLLOUT;

    return events;
}

} // namespace

EpollPoller::EpollPoller(std::size_t maxEventsPerWait)
    : epollFd(epoll_create1(EPOLL_CLOEXEC)), ready(maxEventsPerWait) {
    if (!epollFd.valid())
        throw std::system_error(errno, std::generic_category(),
                                "epoll_create1");
}

bool EpollPoller::add(int fd, bool read, bool write) {
    epoll_event event{};
    event.events = interest(read, write);
    event.data.fd = fd;

    if (epoll_ctl(epollFd.get(), EPOLL_CTL_ADD, fd, &event) < 0)
        return false;

    registered[fd] = event.events;
    return true;
}

void EpollPoller::modify(int fd, bool read, bool write) {
    std::uint32_t wanted = interest(read, write);
    std::uint32_t& current = registered.at(fd);

    if (current == wanted)
        return;

    epoll_event event{};
    event.events = wanted;
    event.data.fd = fd;

    epoll_ctl(epollFd.get(), EPOLL_CTL_MOD, fd, &event);
    current = wanted;
}

void EpollPoller::remove(int fd) {
    epoll_ctl(epollFd.get(), EPOLL_CTL_DEL, fd, nullptr);
    registered.erase(fd);
}

bool EpollPoller::wait(int timeoutMs, std::vector<PollEvent>& events) {
    events.clear();

    int count = epoll_wait(epollFd.get(), ready.data(),
                           static_cast<int>(ready.size()), timeoutMs);

    if (count < 0)
        return errno == EINTR;

    for (int i = 0; i < count; ++i) {
        std::uint32_t happened = ready[static_cast<std::size_t>(i)].events;

        PollEvent event;
        event.fd = ready[static_cast<std::size_t>(i)].data.fd;
        event.readable = (happened & EPOLLIN) != 0;
        event.writable = (happened & EPOLLOUT) != 0;
        event.hangup = (happened & (EPOLLHUP | EPOLLRDHUP)) != 0;
        event.error = (happened & EPOLLERR) != 0;

        events.push_back(event);
    }

    return true;
}

#endif
