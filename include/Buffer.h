#pragma once

#include <cstddef>
#include <memory>
#include <string_view>

// A byte queue: append at the back, consume from the front.
//
// The bytes are kept in one block. Consuming only moves the front
// forward. The space freed that way is reused when the block runs out
// of room at the back: what is left is moved to the start of the block
// if that makes enough room, and the block is replaced by a larger one
// if it does not. The block is never made smaller.
class Buffer {
private:
    std::unique_ptr<char[]> storage;
    std::size_t capacity = 0;

    // The bytes held are storage[first] up to storage[last - 1].
    std::size_t first = 0;
    std::size_t last = 0;

    // Makes sure `count` bytes fit behind the last one.
    void makeRoom(std::size_t count);

public:
    void append(std::string_view bytes);

    // For reading into the buffer without a copy in between. writable()
    // returns room for `count` bytes behind those held, and written()
    // says how many of them were filled in. Nothing else may be called
    // between the two.
    char* writable(std::size_t count);
    void written(std::size_t count);

    // count must not be larger than size().
    void consume(std::size_t count);

    void clear();

    const char* data() const {
        return storage.get() + first;
    }

    std::size_t size() const {
        return last - first;
    }

    std::string_view view() const {
        return {data(), size()};
    }
};
