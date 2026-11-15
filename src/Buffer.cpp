#include "Buffer.h"

#include <algorithm>
#include <cstring>

void Buffer::makeRoom(std::size_t count) {
    if (capacity - last >= count)
        return;

    std::size_t held = size();

    // Moving what is held to the start is enough, and it is no more
    // than was consumed, so a stream of small frames is not shifted
    // over and over.
    if (capacity - held >= count && first >= held) {
        std::memcpy(storage.get(), storage.get() + first, held);
        first = 0;
        last = held;
        return;
    }

    // At least double, so that growing byte by byte copies each byte a
    // limited number of times. The new block is not cleared: every byte
    // is written before it is read.
    std::size_t larger = std::max(capacity * 2, held + count);
    std::unique_ptr<char[]> block(new char[larger]);

    if (held > 0)
        std::memcpy(block.get(), storage.get() + first, held);

    storage = std::move(block);
    capacity = larger;
    first = 0;
    last = held;
}

void Buffer::append(std::string_view bytes) {
    if (bytes.empty())
        return;

    makeRoom(bytes.size());
    std::memcpy(storage.get() + last, bytes.data(), bytes.size());
    last += bytes.size();
}

char* Buffer::writable(std::size_t count) {
    makeRoom(count);
    return storage.get() + last;
}

void Buffer::written(std::size_t count) {
    last += count;
}

void Buffer::consume(std::size_t count) {
    first += count;

    if (first == last)
        clear();
}

void Buffer::clear() {
    first = 0;
    last = 0;
}
