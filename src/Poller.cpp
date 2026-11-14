#include "Poller.h"

#include "EpollPoller.h"
#include "PollPoller.h"

std::unique_ptr<Poller> makePoller(PollerKind kind) {
#ifdef __linux__
    if (kind == PollerKind::Epoll)
        return std::make_unique<EpollPoller>();
#endif

    (void)kind;
    return std::make_unique<PollPoller>();
}
