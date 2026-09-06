#ifndef NETWORK_SERVICE_WPA_EVENT_MONITOR_H
#define NETWORK_SERVICE_WPA_EVENT_MONITOR_H

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace network_service {

struct WpaEventSnapshot {
    bool attached = false;
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
    WpaEventMonitor(std::string iface, std::string ctrl_dir);
    ~WpaEventMonitor();

    WpaEventMonitor(const WpaEventMonitor &) = delete;
    WpaEventMonitor &operator=(const WpaEventMonitor &) = delete;

    void start();
    void stop();
    WpaEventSnapshot snapshot() const;

private:
    void run();
    void update_event(const std::string &event);
    bool handle_connected_event();

    std::string iface_;
    std::string ctrl_dir_;
    std::atomic<bool> running_{false};
    std::thread thread_;
    mutable std::mutex lock_;
    WpaEventSnapshot snapshot_;
};

std::string wpa_event_snapshot_to_json(const WpaEventSnapshot &snapshot);

} // namespace network_service

#endif // NETWORK_SERVICE_WPA_EVENT_MONITOR_H
