#pragma once

#include "FileDescriptor.h"

// Lets another thread, or a signal handler, end the event loop's sleep.
// The loop watches fd() for input. notify() makes it readable, and
// drain() makes it quiet again.
//
// On Linux this is one eventfd, a counter in the kernel made for exactly
// this. Elsewhere it is a pipe: a byte goes in one end and the loop
// watches the other.
class Waker {
private:
    FileDescriptor readEnd;

    // Not used on Linux, where one descriptor does both jobs.
    FileDescriptor writeEnd;

public:
    // Throws std::system_error if the descriptors cannot be created.
    Waker();

    // The descriptor to watch for input.
    int fd() const {
        return readEnd.get();
    }

    // Safe from any thread and from a signal handler. Calling it many
    // times before the loop wakes still wakes the loop once.
    void notify();

    // Called by the loop when fd() is readable.
    void drain();
};
