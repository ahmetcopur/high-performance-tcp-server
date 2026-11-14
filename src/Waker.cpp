#include "Waker.h"

#include <cerrno>
#include <cstdint>
#include <system_error>

#include <unistd.h>

#ifdef __linux__
#include <sys/eventfd.h>
#endif

#ifdef __linux__

Waker::Waker() {
    readEnd.reset(eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));

    if (!readEnd.valid())
        throw std::system_error(errno, std::generic_category(), "eventfd");
}

// errno is put back because this also runs inside a signal handler, on
// top of whatever the thread was doing.
void Waker::notify() {
    int saved = errno;
    std::uint64_t one = 1;
    ssize_t n = write(readEnd.get(), &one, sizeof(one));
    (void)n;
    errno = saved;
}

// One read takes the whole counter, however many times notify() ran.
void Waker::drain() {
    std::uint64_t count;
    ssize_t n = read(readEnd.get(), &count, sizeof(count));
    (void)n;
}

#else

Waker::Waker() {
    int ends[2];

    if (pipe(ends) < 0)
        throw std::system_error(errno, std::generic_category(), "pipe");

    readEnd.reset(ends[0]);
    writeEnd.reset(ends[1]);

    // Non-blocking on both ends: a full pipe must not block the thread
    // that wakes the loop, and emptying it must not block the loop.
    if (!readEnd.setNonBlocking() || !writeEnd.setNonBlocking())
        throw std::system_error(errno, std::generic_category(), "pipe");
}

// If the pipe is full the write fails with EAGAIN, which is fine because
// a wakeup is already pending. errno is put back because this also runs
// inside a signal handler, on top of whatever the thread was doing.
void Waker::notify() {
    int saved = errno;
    char byte = 1;
    ssize_t n = write(writeEnd.get(), &byte, 1);
    (void)n;
    errno = saved;
}

void Waker::drain() {
    char bytes[256];

    while (read(readEnd.get(), bytes, sizeof(bytes)) > 0) {
    }
}

#endif
