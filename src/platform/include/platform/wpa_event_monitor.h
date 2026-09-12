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

// Platform-owned supplicant facts only. IP/DHCP/route/DNS and legacy IPC view
// fields belong to Service projection, not to the monitor.
struct WpaEventFact {
    bool attached = false;
    WifiL2State l2_state = WifiL2State::Unknown;
    bool scan_active = false;
    std::string failure_reason;
    std::uint32_t connect_events = 0;
    std::uint32_t disconnect_events = 0;
    std::uint32_t scan_started_events = 0;
    std::uint32_t scan_result_events = 0;
    std::uint32_t scan_failed_events = 0;
    std::uint64_t event_sequence = 0;
    std::uint64_t last_scan_started_sequence = 0;
    std::uint64_t last_scan_result_sequence = 0;
    std::uint64_t last_scan_failed_sequence = 0;
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
    WpaEventFact snapshot() const;

private:
    void run();
    void update_event(const std::string &event);

    std::string iface_;
    std::string ctrl_dir_;
    LinkStateHandler link_state_handler_;
    std::atomic<bool> running_{false};
    std::thread thread_;
    mutable std::mutex lock_;
    WpaEventFact snapshot_;
};

} // namespace network_service

#endif // NETWORK_SERVICE_WPA_EVENT_MONITOR_H
