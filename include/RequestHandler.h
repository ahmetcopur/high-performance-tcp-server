#pragma once

#include "Protocol.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <utility>

// What a worker thread does with a request. Nothing in here knows about
// sockets or connections.

// Handles ECHO and COMPUTE, the only types the event loop hands to
// workers, and returns the encoded response. A COMPUTE in progress gives
// up when stopWork becomes true.
std::string processRequest(protocol::Frame request,
                           const std::atomic<bool>& stopWork);

// Runs work and returns what it returns. If work throws, returns an
// encoded ERROR 6 (internal error) for the request instead. A failure
// inside one request then reaches the client as an answer, and the
// connection goes on.
template<typename Work>
std::string answerOrInternalError(std::uint32_t requestId, Work&& work) {
    try {
        return std::forward<Work>(work)();
    } catch (...) {
        return protocol::encodeError(
            requestId, protocol::ErrorCode::InternalError,
            protocol::errorName(protocol::ErrorCode::InternalError));
    }
}
