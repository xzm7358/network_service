#ifndef NETWORK_SERVICE_WPA_EVENT_MONITOR_H
#define NETWORK_SERVICE_WPA_EVENT_MONITOR_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "network_service_types.h"

namespace network_service {

struct WpaEventSnapshot {
    bool attached = false;

    // L2 truth owned by the supplicant monitor. Scan is deliberately separate:
    // a connected station may perform background scans without losing L2.
    WifiL2State l2_state = WifiL2State::Unknown;
    bool scan_active = false;

    // Legacy compatibility fields retained as typed data for IPC projection.
    bool connected = false;
    bool disconnected = false;
    bool dhcp_requested = false;
    std::string wifi_state = "disconnected";
    std::string failure_reason;
    std::string ip4;
    std::string gateway4;
    std::string dns4;
    bool has_ip = false;
    bool has_default_route = false;
    bool dns_available = false;
    uint32_t connect_events = 0;
    uint32_t disconnect_events = 0;
    uint32_t dhcp_requests = 0;
    uint32_t scan_started_events = 0;
    uint32_t scan_result_events = 0;
    uint32_t scan_failed_events = 0;
    uint64_t event_sequence = 0;
    uint64_t last_scan_started_sequence = 0;
    uint64_t last_scan_result_sequence = 0;
    uint64_t last_scan_failed_sequence = 0;
    std::string last_event;
    std::string last_ssid;
    std::string last_bssid;
};

class WpaEventMonitor {
public:
    using LinkStateHandler = std::function<void(bool connected)>;

    WpaEventMonitor(std::string iface,
                    std::string ctrl_dir,
                    LinkStateHandler link_state_handler = {});
    ~WpaEventMonitor();

    WpaEventMonitor(const WpaEventMonitor &) = delete;
    WpaEventMonitor &operator=(const WpaEventMonitor &) = delete;

    void start();
    void stop();
    WpaEventSnapshot snapshot() const;

private:
    void run();
    void update_event(const std::string &event);

    std::string iface_;
    std::string ctrl_dir_;
    LinkStateHandler link_state_handler_;
    std::atomic<bool> running_{false};
    std::thread thread_;
    mutable std::mutex lock_;
    WpaEventSnapshot snapshot_;
};

} // namespace network_service

#endif // NETWORK_SERVICE_WPA_EVENT_MONITOR_H
