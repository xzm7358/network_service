#ifndef NETWORK_SERVICE_CONTRACTS_H
#define NETWORK_SERVICE_CONTRACTS_H

#include <sstream>
#include <string>

namespace network_service {

// Cross-boundary data contracts shared by Service, IPC, and Platform adapters.
// These types are intentionally mechanism-neutral and must not depend on
// Platform/Service/IPC implementation headers.
struct EthernetConfig {
    std::string iface = "eth0";
    std::string method = "dhcp";
    std::string ip4;
    std::string netmask4;
    std::string gateway4;
    std::string dns4;
    int route_metric = 10;
    bool dns_enabled = true;
};

struct WifiApRecord {
    std::string bssid;
    int frequency = 0;
    int signal_dbm = 0;
    std::string flags;
    std::string ssid;
};

struct WifiSavedNetwork {
    int network_id = -1;
    std::string ssid;
    std::string bssid;
    std::string flags;
    bool is_current = false;
    bool is_disabled = false;
    bool is_temp_disabled = false;
    bool autoconnect = true;
};

enum class DhcpLeaseEvent {
    None = 0,
    Bound,
    Renew,
    Deconfig,
};

struct DhcpLeaseFact {
    DhcpLeaseEvent event = DhcpLeaseEvent::None;
    std::string iface;
    std::string generation;
    std::string ip4;
    std::string netmask4;
    std::string gateway4;
    std::string dns4;

    bool configured() const {
        return event == DhcpLeaseEvent::Bound || event == DhcpLeaseEvent::Renew;
    }

    std::string fingerprint() const {
        std::ostringstream os;
        os << static_cast<int>(event) << '\x1f'
           << iface << '\x1f'
           << generation << '\x1f'
           << ip4 << '\x1f'
           << netmask4 << '\x1f'
           << gateway4 << '\x1f'
           << dns4;
        return os.str();
    }
};

} // namespace network_service

#endif // NETWORK_SERVICE_CONTRACTS_H
