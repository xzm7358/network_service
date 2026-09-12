#ifndef NETWORK_SERVICE_WPA_CTRL_CLIENT_H
#define NETWORK_SERVICE_WPA_CTRL_CLIENT_H

#include <string>

namespace network_service {

// Thin owner for one wpa_supplicant control socket connection.
//
// The service layer must not depend on wpa_supplicant control protocol details.
// This adapter intentionally mirrors the small subset of wpa_ctrl semantics used
// by NetworkService so the implementation can later be swapped to the SDK's
// official wpa_ctrl without changing callers.
class WpaCtrlClient {
public:
    explicit WpaCtrlClient(std::string ctrl_path);
    ~WpaCtrlClient();

    WpaCtrlClient(const WpaCtrlClient &) = delete;
    WpaCtrlClient &operator=(const WpaCtrlClient &) = delete;

    bool open(std::string &error);
    void close();
    bool is_open() const;

    bool request(const std::string &command,
                 std::string &reply,
                 std::string &error,
                 int timeout_ms = 2000);

    bool attach(std::string &error);
    void detach();

    int fd() const;
    bool receive(std::string &message, std::string &error);

private:
    std::string ctrl_path_;
    std::string local_path_;
    int fd_ = -1;
    bool attached_ = false;
};

std::string wpa_ctrl_path_for(const std::string &iface,
                              const std::string &ctrl_dir = {});

} // namespace network_service

#endif // NETWORK_SERVICE_WPA_CTRL_CLIENT_H
