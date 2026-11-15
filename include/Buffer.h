#pragma once

#include <cstddef>
#include <string>
#include <string_view>

// A byte queue: append at the back, consume from the front.
//
// Consuming only advances an offset. The consumed bytes are dropped later
// in one move, once they are at least half of the storage, so a stream of
// small frames does not shift the whole buffer every time.
class Buffer {
private:
    std::string storage;
    std::size_t offset = 0;

public:
    void append(std::string_view bytes);

    // count must not be larger than size().
    void consume(std::size_t count);

    void clear();

    const char* data() const {
        return storage.data() + offset;
    }

    std::size_t size() const {
        return storage.size() - offset;
    }

    std::string_view view() const {
        return {data(), size()};
    }
};
