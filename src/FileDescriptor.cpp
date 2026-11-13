#include "FileDescriptor.h"

#include <unistd.h>

FileDescriptor& FileDescriptor::operator=(FileDescriptor&& other) noexcept {
    if (this != &other) {
        reset(other.fd);
        other.fd = -1;
    }

    return *this;
}

void FileDescriptor::reset(int descriptor) {
    if (fd >= 0)
        close(fd);

    fd = descriptor;
}
