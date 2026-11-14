#include "FileDescriptor.h"

#include <fcntl.h>
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

bool FileDescriptor::setNonBlocking() {
    int flags = fcntl(fd, F_GETFL, 0);

    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) >= 0;
}
