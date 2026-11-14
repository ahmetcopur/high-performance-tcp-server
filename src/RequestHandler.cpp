#include "RequestHandler.h"

#include <string_view>

std::string processRequest(protocol::Frame request,
                           const std::atomic<bool>& stopWork) {
    using namespace protocol;

    Frame response;
    response.type = static_cast<std::uint8_t>(request.type | RESPONSE_BIT);
    response.requestId = request.requestId;

    if (request.type == TYPE_ECHO) {
        response.payload = std::move(request.payload);
        return encodeFrame(response);
    }

    if (request.payload.size() < 4)
        return encodeError(request.requestId, ErrorCode::BadRequest,
                           "compute payload shorter than 4 bytes");

    std::uint32_t iterations = readU32(request.payload.data());

    if (iterations > MAX_COMPUTE_ITERATIONS)
        return encodeError(request.requestId, ErrorCode::BadRequest,
                           "iteration count above the limit");

    std::string_view data(request.payload.data() + 4,
                          request.payload.size() - 4);

    appendU64(response.payload, computeHash(data, iterations, &stopWork));
    return encodeFrame(response);
}
