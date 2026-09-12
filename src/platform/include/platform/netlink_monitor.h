#ifndef NETWORK_SERVICE_NETLINK_MONITOR_H
#define NETWORK_SERVICE_NETLINK_MONITOR_H

#include <cstddef>
#include <cstdint>
#include <string>

namespace network_service {

class NetlinkMonitor {
public:
    NetlinkMonitor() = default;
    ~NetlinkMonitor();

    NetlinkMonitor(const NetlinkMonitor &) = delete;
    NetlinkMonitor &operator=(const NetlinkMonitor &) = delete;

    bool open(std::string &error);
    void close();
    int fd() const;

    // Drains all currently queued Netlink messages. `changed` means at least
    // one link, IPv4 address, or default IPv4 route fact may have changed.
    bool drain(bool &changed, std::string &error);

    // Pure classifier used by host regressions. `payload` points at the message
    // body immediately after nlmsghdr.
    static bool affects_network_state(std::uint16_t message_type,
                                      const void *payload,
                                      std::size_t payload_size);

private:
    int fd_ = -1;
};

} // namespace network_service

#endif // NETWORK_SERVICE_NETLINK_MONITOR_H
