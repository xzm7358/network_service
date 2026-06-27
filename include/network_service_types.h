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
    bool connected = false;
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
    bool online = false;
    std::string dns4;
};

} // namespace network_service

#endif // NETWORK_SERVICE_TYPES_H
