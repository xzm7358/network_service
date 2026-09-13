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

// Current product DNS semantics are intentionally singular: DNS follows the
// selected primary route and NetworkService overwrites only its owned resolver
// file. Append/Disabled previously existed as dead enum surface with no policy
// implementation; keep the wire field but expose only the behavior that exists.
enum class DnsPolicy {
    Overwrite,
};

enum class WifiL2State {
    Unknown = 0,
    Disabled,
    Disconnected,
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

// Service-owned DHCP client lifecycle truth. This is deliberately independent
// from lease/IP truth: a client can exit after configuring an address, and a
// running client does not imply that an address has been acquired yet.
enum class DhcpClientState {
    Idle = 0,
    Starting,
    Running,
    Failed,
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
    // is meaningful only for the Wi-Fi interface. Scanning is intentionally not
    // an L2 state; it belongs to the independent Wi-Fi scan lifecycle.
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

    // Raw filesystem ownership fact. Platform sets this only when the resolver
    // file carries NetworkService's exact ownership marker. Service decides
    // whether to adopt/relinquish that historical ownership. This field is
    // internal truth and is deliberately not part of the IPC wire schema.
    bool dns_managed_by_network_service = false;

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
