#include "FileDescriptor.h"

#include <gtest/gtest.h>

#include <utility>

#include <fcntl.h>
#include <unistd.h>

namespace {

int openNull() {
    return open("/dev/null", O_RDONLY);
}

bool isOpen(int fd) {
    return fcntl(fd, F_GETFD) != -1;
}

} // namespace

TEST(FileDescriptor, StartsWithoutADescriptor) {
    FileDescriptor descriptor;

    EXPECT_FALSE(descriptor.valid());
    EXPECT_EQ(descriptor.get(), -1);
}

TEST(FileDescriptor, ClosesWhenDestroyed) {
    int fd = openNull();
    ASSERT_GE(fd, 0);

    {
        FileDescriptor descriptor(fd);

        EXPECT_TRUE(descriptor.valid());
        EXPECT_EQ(descriptor.get(), fd);
        EXPECT_TRUE(isOpen(fd));
    }

    EXPECT_FALSE(isOpen(fd));
}

TEST(FileDescriptor, MoveConstructionTransfersOwnership) {
    int fd = openNull();
    FileDescriptor first(fd);

    FileDescriptor second(std::move(first));

    EXPECT_FALSE(first.valid());
    EXPECT_EQ(second.get(), fd);
    EXPECT_TRUE(isOpen(fd));
}

TEST(FileDescriptor, MoveAssignmentClosesWhatWasHeld) {
    int kept = openNull();
    int replaced = openNull();
    FileDescriptor source(kept);
    FileDescriptor target(replaced);

    target = std::move(source);

    EXPECT_FALSE(isOpen(replaced));
    EXPECT_TRUE(isOpen(kept));
    EXPECT_EQ(target.get(), kept);
    EXPECT_FALSE(source.valid());
}

TEST(FileDescriptor, ResetClosesAndTakesTheNewOne) {
    int first = openNull();
    FileDescriptor descriptor(first);

    // Opened before the reset, so it cannot be given the number that
    // the reset is about to free.
    int second = openNull();
    descriptor.reset(second);

    EXPECT_FALSE(isOpen(first));
    EXPECT_EQ(descriptor.get(), second);

    descriptor.reset();

    EXPECT_FALSE(isOpen(second));
    EXPECT_FALSE(descriptor.valid());
}

TEST(FileDescriptor, SetNonBlockingSetsTheFlag) {
    FileDescriptor descriptor(openNull());
    ASSERT_EQ(fcntl(descriptor.get(), F_GETFL) & O_NONBLOCK, 0);

    EXPECT_TRUE(descriptor.setNonBlocking());

    EXPECT_NE(fcntl(descriptor.get(), F_GETFL) & O_NONBLOCK, 0);
}

TEST(FileDescriptor, SetNonBlockingFailsWithoutADescriptor) {
    FileDescriptor descriptor;

    EXPECT_FALSE(descriptor.setNonBlocking());
}
