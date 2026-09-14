#ifndef NETWORK_SERVICE_V1_CLIENT_H
#define NETWORK_SERVICE_V1_CLIENT_H

#include <cstdint>
#include <memory>
#include <string>

namespace network_service {

class NetworkServiceClient {
public:
    NetworkServiceClient();
    ~NetworkServiceClient();
    NetworkServiceClient(const NetworkServiceClient &) = delete;
    NetworkServiceClient &operator=(const NetworkServiceClient &) = delete;

    bool connectTo(const std::string &socket_path, std::string &error,
                   int timeout_ms = 1500);
    void close();
    int fd() const;
    bool request(const std::string &method, const std::string &params_json,
                 std::string &response_json, std::string &error,
                 int timeout_ms = 5000);
    bool readNext(std::string &event_json, std::string &error);

    std::uint64_t generation() const;
    std::uint64_t snapshotSequence() const;
    bool requiresSnapshotRebase() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

std::string jsonQuote(const std::string &value);

} // namespace network_service

#endif // NETWORK_SERVICE_V1_CLIENT_H
