#ifndef NETWORK_SERVICE_NETWORK_OPERATION_RESULT_H
#define NETWORK_SERVICE_NETWORK_OPERATION_RESULT_H

#include <string>
#include <utility>

namespace network_service {

template <typename T>
struct NetworkOperationResult {
    int status = 200;
    T value{};
    std::string error;

    bool ok() const { return status >= 200 && status < 300; }

    static NetworkOperationResult success(T value, int status = 200) {
        NetworkOperationResult result;
        result.status = status;
        result.value = std::move(value);
        return result;
    }

    static NetworkOperationResult failure(int status, std::string error) {
        NetworkOperationResult result;
        result.status = status;
        result.error = std::move(error);
        return result;
    }
};

struct PingInfo {
    std::string service;
    std::string version;
    std::string mode;
};

struct WifiEnabledResult {
    bool enabled = false;
};

struct WifiCommandResult {
    std::string requested;
    bool has_enabled = false;
    bool enabled = false;
};

} // namespace network_service

#endif // NETWORK_SERVICE_NETWORK_OPERATION_RESULT_H
