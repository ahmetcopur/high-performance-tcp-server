#include "Protocol.h"

namespace protocol {

namespace {

std::uint8_t byteAt(const char* bytes, std::size_t index) {
    return static_cast<std::uint8_t>(bytes[index]);
}

} // namespace

void appendU16(std::string& out, std::uint16_t value) {
    out.push_back(static_cast<char>(value >> 8));
    out.push_back(static_cast<char>(value));
}

void appendU32(std::string& out, std::uint32_t value) {
    out.push_back(static_cast<char>(value >> 24));
    out.push_back(static_cast<char>(value >> 16));
    out.push_back(static_cast<char>(value >> 8));
    out.push_back(static_cast<char>(value));
}

void appendU64(std::string& out, std::uint64_t value) {
    appendU32(out, static_cast<std::uint32_t>(value >> 32));
    appendU32(out, static_cast<std::uint32_t>(value));
}

std::uint16_t readU16(const char* bytes) {
    return static_cast<std::uint16_t>(byteAt(bytes, 0) << 8 | byteAt(bytes, 1));
}

std::uint32_t readU32(const char* bytes) {
    return static_cast<std::uint32_t>(byteAt(bytes, 0)) << 24 |
           static_cast<std::uint32_t>(byteAt(bytes, 1)) << 16 |
           static_cast<std::uint32_t>(byteAt(bytes, 2)) << 8 |
           static_cast<std::uint32_t>(byteAt(bytes, 3));
}

std::uint64_t readU64(const char* bytes) {
    return static_cast<std::uint64_t>(readU32(bytes)) << 32 |
           readU32(bytes + 4);
}

const char* errorName(ErrorCode code) {
    switch (code) {
    case ErrorCode::MalformedFrame:
        return "malformed frame";
    case ErrorCode::UnsupportedVersion:
        return "unsupported version";
    case ErrorCode::UnknownType:
        return "unknown type";
    case ErrorCode::PayloadTooLarge:
        return "payload too large";
    case ErrorCode::ServerBusy:
        return "server busy";
    case ErrorCode::InternalError:
        return "internal error";
    case ErrorCode::BadRequest:
        return "bad request";
    }

    return "unknown error";
}

std::string encodeFrame(const Frame& frame) {
    std::string out;
    out.reserve(HEADER_SIZE + frame.payload.size());

    appendU32(out,
              static_cast<std::uint32_t>(MIN_LENGTH + frame.payload.size()));
    out.push_back(static_cast<char>(frame.version));
    out.push_back(static_cast<char>(frame.type));
    appendU16(out, frame.reserved);
    appendU32(out, frame.requestId);
    out.append(frame.payload);

    return out;
}

std::string encodeError(std::uint32_t requestId, ErrorCode code,
                        std::string_view message) {
    Frame frame;
    frame.type = TYPE_ERROR;
    frame.requestId = requestId;

    appendU16(frame.payload, static_cast<std::uint16_t>(code));
    frame.payload.append(message);

    return encodeFrame(frame);
}

DecodeResult decodeFrame(std::string_view data, std::uint32_t maxLength) {
    DecodeResult result;

    if (data.size() < LENGTH_SIZE)
        return result;

    std::uint32_t length = readU32(data.data());

    // Decided from the first four bytes alone, so an oversized frame is
    // refused before any of it is buffered.
    if (length < MIN_LENGTH) {
        result.status = DecodeStatus::Invalid;
        result.error = ErrorCode::MalformedFrame;
        return result;
    }

    if (length > maxLength) {
        result.status = DecodeStatus::Invalid;
        result.error = ErrorCode::PayloadTooLarge;
        return result;
    }

    if (data.size() - LENGTH_SIZE < length)
        return result;

    const char* bytes = data.data();

    result.status = DecodeStatus::Complete;
    result.consumed = LENGTH_SIZE + length;
    result.frame.version = byteAt(bytes, 4);
    result.frame.type = byteAt(bytes, 5);
    result.frame.reserved = readU16(bytes + 6);
    result.frame.requestId = readU32(bytes + 8);
    result.frame.payload.assign(bytes + HEADER_SIZE, length - MIN_LENGTH);

    return result;
}

bool validateRequest(const Frame& frame, ErrorCode& error) {
    // First, because the other fields mean nothing in an unknown version.
    if (frame.version != VERSION) {
        error = ErrorCode::UnsupportedVersion;
        return false;
    }

    if (frame.reserved != 0) {
        error = ErrorCode::MalformedFrame;
        return false;
    }

    if (frame.type != TYPE_PING && frame.type != TYPE_ECHO &&
        frame.type != TYPE_COMPUTE) {
        error = ErrorCode::UnknownType;
        return false;
    }

    return true;
}

std::uint64_t computeHash(std::string_view data, std::uint32_t iterations) {
    std::uint64_t hash = 0xcbf29ce484222325ull;

    for (std::uint32_t i = 0; i < iterations; ++i) {
        for (char c : data) {
            hash ^= static_cast<std::uint8_t>(c);
            hash *= 0x100000001b3ull;
        }
    }

    return hash;
}

} // namespace protocol
