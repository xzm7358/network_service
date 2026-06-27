#ifndef NETWORK_SERVICE_PROTOCOL_H
#define NETWORK_SERVICE_PROTOCOL_H

namespace network_service {

constexpr const char *kDefaultSocketPath = "/tmp/smart_hmi_network.sock";
constexpr const char *kMethodPing = "network.ping";
constexpr const char *kMethodSnapshot = "network.snapshot";
constexpr const char *kMethodSubscribe = "network.subscribe";

constexpr int kStatusOk = 200;
constexpr int kStatusBadRequest = 400;
constexpr int kStatusNotFound = 404;
constexpr int kStatusInternalError = 500;

} // namespace network_service

#endif // NETWORK_SERVICE_PROTOCOL_H
