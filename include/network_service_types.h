#ifndef NETWORK_SERVICE_TYPES_H
#define NETWORK_SERVICE_TYPES_H

#include <cstdint>
#include <string>

namespace network_service {

enum class IpMethod {
    Dhcp,
    Static,
    Disabled,
};

enum class RoutePolicy {
    EthernetPreferred,
    WifiPreferred,
    WifiOnly,
    ManualMetric,
};

enum class DnsPolicy {
    Overwrite,
    Append,
    Disabled,
};

enum class WifiL2State {
    Unknown = 0,
    Disabled,
    Disconnected,
    Scanning,
    Associating,
    Associated,
    Handshake,
    Connected,
    Failed,
};

enum class IpState {
    None = 0,
    Configuring,
    Ready,
};

struct InterfaceSnapshot {
    std::string iface;
    bool exists = false;
    bool carrier_up = false;
    bool has_ip = false;
    std::string ip4;
    std::string netmask4;
    bool has_default_route = false;
    std::string gateway4;
    int route_metric = -1;
    bool enabled = false;

    // Legacy compatibility projection. Production Service/Policy code must use
    // the typed truth fields below instead of inferring link truth from this bool.
    bool connected = false;

    // Internal truth. ip_state is meaningful for both interfaces; wifi_l2_state
    // is meaningful only for the Wi-Fi interface.
    IpState ip_state = IpState::None;
    WifiL2State wifi_l2_state = WifiL2State::Unknown;

    std::string ssid;
    int signal_dbm = 0;
    int signal_bars = 0;
};

struct NetworkSnapshot {
    InterfaceSnapshot eth;
    InterfaceSnapshot wifi;
    RoutePolicy route_policy = RoutePolicy::EthernetPreferred;
    DnsPolicy dns_policy = DnsPolicy::Overwrite;
    std::string primary_iface;
    bool dns_available = false;

    // Internal readiness fact: the selected primary interface has usable link,
    // IPv4/default-route truth and DNS configuration. This is not an Internet
    // reachability probe.
    bool network_ready = false;

    // Legacy IPC compatibility field. Serialized as before, but derived from
    // network_ready by the Service truth normalizer.
    bool online = false;
    std::string dns4;
};

} // namespace network_service

#endif // NETWORK_SERVICE_TYPES_H
