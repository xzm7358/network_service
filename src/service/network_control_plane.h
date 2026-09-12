#ifndef NETWORK_SERVICE_NETWORK_CONTROL_PLANE_H
#define NETWORK_SERVICE_NETWORK_CONTROL_PLANE_H

#include <functional>
#include <mutex>
#include <string>

#include "network_service_types.h"
#include "platform/dhcp_lease_store.h"

namespace network_service {

struct NetworkControlPlaneOps {
    std::function<bool(const std::string &, std::string &)> start_dhcp;
    std::function<void(const std::string &)> stop_dhcp;
    std::function<bool(const std::string &, DhcpLeaseFact &, bool &, std::string &)> read_lease;
    std::function<void(const std::string &)> clear_lease;
    std::function<bool(const std::string &, const std::string &, const std::string &, std::string &)> apply_ipv4;
    std::function<bool(const std::string &, std::string &)> clear_ipv4;
    std::function<bool(const std::string &, const std::string &, int, std::string &)> set_default_route;
    std::function<void(const std::string &)> clear_default_route;
    std::function<bool(const std::string &, std::string &)> set_dns;
    std::function<bool(std::string &)> clear_dns;
    std::function<NetworkSnapshot()> snapshot;
};

class NetworkControlPlane {
public:
    NetworkControlPlane(std::string eth_iface,
                        std::string wifi_iface,
                        NetworkControlPlaneOps ops);

    bool start_dhcp(const std::string &iface, std::string &error);
    void stop_dhcp(const std::string &iface);

    bool apply_ethernet_static(const std::string &ip4,
                               const std::string &netmask4,
                               const std::string &gateway4,
                               const std::string &dns4,
                               int manual_metric,
                               std::string &error);

    bool reconcile(std::string &error);
    bool set_route_policy(RoutePolicy policy, std::string &error);
    RoutePolicy route_policy() const;

private:
    struct LinkState {
        bool managed_dhcp = false;
        bool active = false;
        DhcpLeaseFact lease;
        std::string fingerprint;
    };

    struct StaticEthernetState {
        bool active = false;
        std::string gateway4;
        std::string dns4;
        int manual_metric = 10;
    };

    bool known_iface(const std::string &iface) const;
    LinkState &link_for(const std::string &iface);
    const LinkState &link_for(const std::string &iface) const;
    bool reconcile_link_locked(const std::string &iface,
                               LinkState &state,
                               std::string &error);
    bool apply_routes_locked(std::string &error);
    bool recompute_dns_locked(std::string &error);
    bool route_allowed(const std::string &iface) const;
    int route_metric(const std::string &iface, int manual_metric = -1) const;

    std::string eth_iface_;
    std::string wifi_iface_;
    NetworkControlPlaneOps ops_;
    mutable std::mutex lock_;
    LinkState eth_;
    LinkState wifi_;
    StaticEthernetState static_eth_;
    RoutePolicy route_policy_ = RoutePolicy::EthernetPreferred;
    bool managed_dns_ = false;
    std::string last_dns_;
};

} // namespace network_service

#endif // NETWORK_SERVICE_NETWORK_CONTROL_PLANE_H
