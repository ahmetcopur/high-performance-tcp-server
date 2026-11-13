#pragma once

// Owns one file descriptor and closes it when destroyed. It can be moved
// but not copied, so a descriptor has exactly one owner at any time and
// is closed exactly once. Nothing else in the project calls close().
class FileDescriptor {
private:
    int fd = -1;

public:
    FileDescriptor() = default;

    explicit FileDescriptor(int descriptor) : fd(descriptor) {
    }

    ~FileDescriptor() {
        reset();
    }

    FileDescriptor(FileDescriptor&& other) noexcept : fd(other.fd) {
        other.fd = -1;
    }

    FileDescriptor& operator=(FileDescriptor&& other) noexcept;

    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;

    int get() const {
        return fd;
    }

    bool valid() const {
        return fd >= 0;
    }

    // Closes the descriptor held, if any, and takes over the new one.
    void reset(int descriptor = -1);
};
