#include "Buffer.h"

void Buffer::append(std::string_view bytes) {
    storage.append(bytes);
}

void Buffer::consume(std::size_t count) {
    offset += count;

    if (offset == storage.size()) {
        clear();
    }
    else if (offset >= storage.size() / 2) {
        storage.erase(0, offset);
        offset = 0;
    }
}

void Buffer::clear() {
    storage.clear();
    offset = 0;
}
