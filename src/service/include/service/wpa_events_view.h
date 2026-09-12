#ifndef NETWORK_SERVICE_WPA_EVENTS_VIEW_H
#define NETWORK_SERVICE_WPA_EVENTS_VIEW_H

#include <cstdint>
#include <string>

namespace network_service {

// Service-owned compatibility projection for the existing wpa.events IPC shape.
// Platform WpaEventMonitor owns only supplicant/L2 facts and never populates
// IP/DHCP/route/DNS compatibility state.
struct WpaEventsView {
    bool attached = false;
    bool connected = false;
    bool disconnected = false;
    bool dhcp_requested = false;
    bool has_ip = false;
    bool has_default_route = false;
    bool dns_available = false;
    std::string ip4;
    std::string gateway4;
    std::string dns4;
    std::uint32_t connect_events = 0;
    std::uint32_t disconnect_events = 0;
    std::uint32_t dhcp_requests = 0;
    std::uint32_t scan_started_events = 0;
    std::uint32_t scan_result_events = 0;
    std::uint32_t scan_failed_events = 0;
    std::uint64_t event_sequence = 0;
    std::uint64_t last_scan_started_sequence = 0;
    std::uint64_t last_scan_result_sequence = 0;
    std::uint64_t last_scan_failed_sequence = 0;
    std::string wifi_state = "disconnected";
    std::string failure_reason;
    std::string last_event;
    std::string last_ssid;
    std::string last_bssid;
};

} // namespace network_service

#endif // NETWORK_SERVICE_WPA_EVENTS_VIEW_H
