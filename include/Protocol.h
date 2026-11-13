#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// Wire format v1. The full description is in docs/protocol.md.
//
//   offset  size  field
//   0       4     length      uint32 big-endian, bytes after this field
//   4       1     version     1
//   5       1     type
//   6       2     reserved    0
//   8       4     request_id  uint32 big-endian, chosen by the client
//   12      n     payload     n = length - 8
//
// Nothing in here touches a socket.
namespace protocol {

constexpr std::uint8_t VERSION = 1;

constexpr std::size_t LENGTH_SIZE = 4;
constexpr std::uint32_t MIN_LENGTH = 8;
constexpr std::size_t HEADER_SIZE = LENGTH_SIZE + MIN_LENGTH;

constexpr std::uint8_t TYPE_PING = 1;
constexpr std::uint8_t TYPE_ECHO = 2;
constexpr std::uint8_t TYPE_COMPUTE = 3;
constexpr std::uint8_t TYPE_STATS = 4;
constexpr std::uint8_t TYPE_ERROR = 0xFF;

// A response carries the type of its request with this bit set.
constexpr std::uint8_t RESPONSE_BIT = 0x80;

constexpr std::uint32_t MAX_COMPUTE_ITERATIONS = 100000;

enum class ErrorCode : std::uint16_t {
    MalformedFrame = 1,
    UnsupportedVersion = 2,
    UnknownType = 3,
    PayloadTooLarge = 4,
    ServerBusy = 5,
    InternalError = 6,
    BadRequest = 7,
};

struct Frame {
    std::uint8_t version = VERSION;
    std::uint8_t type = 0;
    std::uint16_t reserved = 0;
    std::uint32_t requestId = 0;
    std::string payload;
};

// Big-endian integers, most significant byte first.
void appendU16(std::string& out, std::uint16_t value);
void appendU32(std::string& out, std::uint32_t value);
void appendU64(std::string& out, std::uint64_t value);

std::uint16_t readU16(const char* bytes);
std::uint32_t readU32(const char* bytes);
std::uint64_t readU64(const char* bytes);

// The standard short text for a code, for example "unknown type".
const char* errorName(ErrorCode code);

// Writes the frame exactly as given, including a wrong version or a
// nonzero reserved field. The length field is computed.
std::string encodeFrame(const Frame& frame);

// An ERROR frame. The payload is the code followed by the message.
std::string encodeError(std::uint32_t requestId, ErrorCode code,
                        std::string_view message);

enum class DecodeStatus {
    Complete,   // frame holds the first frame in the data
    Incomplete, // the data ends before the first frame does
    Invalid,    // the length field is unusable, the stream is lost
};

struct DecodeResult {
    DecodeStatus status = DecodeStatus::Incomplete;
    std::size_t consumed = 0;
    ErrorCode error = ErrorCode::MalformedFrame;
    Frame frame;
};

// Looks at the front of data for one frame. maxLength is the largest
// value the length field may have.
//
// Only the length is judged here, because it alone decides where the
// next frame starts. Version, type and reserved are returned as they
// arrived, see validateRequest().
DecodeResult decodeFrame(std::string_view data, std::uint32_t maxLength);

// Checks the fields that do not affect framing. Returns false and sets
// error when the frame must be answered with an ERROR. The connection
// can go on either way, since the frame boundary is known.
bool validateRequest(const Frame& frame, ErrorCode& error);

// The work behind COMPUTE: 64-bit FNV-1a over data, passing over it
// `iterations` times without resetting the hash in between.
//
// If cancelled is given, it is looked at before every pass, and the
// function returns early once it is true. The value returned then is
// meaningless and must not be sent to anyone.
std::uint64_t computeHash(std::string_view data, std::uint32_t iterations,
                          const std::atomic<bool>* cancelled = nullptr);

} // namespace protocol
