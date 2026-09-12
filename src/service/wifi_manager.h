#ifndef NETWORK_SERVICE_WIFI_MANAGER_H
#define NETWORK_SERVICE_WIFI_MANAGER_H

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

namespace network_service {

struct WifiManagerState {
    bool l2_connected = false;
    bool dhcp_requested = false;
    std::uint32_t dhcp_requests = 0;
    std::string failure_reason;
};

class WifiManager {
public:
    using DhcpStartFn = std::function<bool(std::string &error)>;
    using DhcpStopFn = std::function<void()>;

    WifiManager(DhcpStartFn dhcp_start, DhcpStopFn dhcp_stop);

    void on_l2_connected();
    void on_l2_disconnected();
    void stop_dhcp();

    WifiManagerState state() const;

private:
    DhcpStartFn dhcp_start_;
    DhcpStopFn dhcp_stop_;

    // lock_ protects state only. External DHCP callbacks are never invoked while
    // holding it, so ControlPlane reconciliation may safely read manager state.
    mutable std::mutex lock_;
    WifiManagerState state_;

    // Serializes start/stop mechanism calls without participating in the state
    // lock order. This prevents concurrent CONNECTED/DISCONNECTED operations
    // from spawning or leaving duplicate udhcpc instances.
    std::mutex operation_lock_;
};

} // namespace network_service

#endif // NETWORK_SERVICE_WIFI_MANAGER_H
