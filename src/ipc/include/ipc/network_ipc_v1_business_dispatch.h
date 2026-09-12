#ifndef NETWORK_IPC_V1_BUSINESS_DISPATCH_H
#define NETWORK_IPC_V1_BUSINESS_DISPATCH_H

#include <cstdint>
#include <string>
#include <vector>

namespace network_service {

class NetworkDaemon;

namespace ipc_v1 {

// Server-side adapter between the protocol-only Session and NetworkDaemon.
// Returns one fully encoded IPC v1 RESPONSE frame. Application failures stay
// correlated to request_id and do not imply session teardown.
std::vector<std::uint8_t> dispatch_business_request(
    NetworkDaemon &daemon,
    std::uint64_t request_id,
    const std::string &method,
    const std::string &params_json);

} // namespace ipc_v1
} // namespace network_service

#endif // NETWORK_IPC_V1_BUSINESS_DISPATCH_H
