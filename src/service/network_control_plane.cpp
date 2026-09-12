#include "service/network_control_plane.h"

#include <utility>

namespace network_service {

NetworkControlPlane::NetworkControlPlane(std::string eth_iface,
                                         std::string wifi_iface,
                                         NetworkControlPlaneOps ops)
    : eth_iface_(std::move(eth_iface)),
      wifi_iface_(std::move(wifi_iface)),
      ops_(std::move(ops)) {}

bool NetworkControlPlane::known_iface(const std::string &iface) const {
    return iface == eth_iface_ || iface == wifi_iface_;
}

NetworkControlPlane::LinkState &NetworkControlPlane::link_for(const std::string &iface) {
    return iface == eth_iface_ ? eth_ : wifi_;
}

const NetworkControlPlane::LinkState &NetworkControlPlane::link_for(const std::string &iface) const {
    return iface == eth_iface_ ? eth_ : wifi_;
}

bool NetworkControlPlane::route_allowed(const std::string &iface) const {
    return !(route_policy_ == RoutePolicy::WifiOnly && iface == eth_iface_);
}

int NetworkControlPlane::route_metric(const std::string &iface, int manual_metric) const {
    switch (route_policy_) {
    case RoutePolicy::WifiPreferred:
        return iface == wifi_iface_ ? 10 : 20;
    case RoutePolicy::WifiOnly:
        return iface == wifi_iface_ ? 10 : 20;
    case RoutePolicy::ManualMetric:
        if (manual_metric >= 0) return manual_metric;
        return iface == eth_iface_ ? 10 : 20;
    case RoutePolicy::EthernetPreferred:
    default:
        return iface == eth_iface_ ? 10 : 20;
    }
}

bool NetworkControlPlane::start_dhcp(const std::string &iface, std::string &error) {
    std::lock_guard<std::mutex> guard(lock_);
    error.clear();
    if (!known_iface(iface)) {
        error = "unknown DHCP interface";
        return false;
    }
    if (!ops_.start_dhcp || !ops_.stop_dhcp || !ops_.clear_lease) {
        error = "DHCP platform ports are unavailable";
        return false;
    }

    LinkState &state = link_for(iface);
    ops_.stop_dhcp(iface);
    ops_.clear_lease(iface);
    if (ops_.clear_default_route) ops_.clear_default_route(iface);
    if (ops_.clear_ipv4) {
        std::string ignored;
        (void)ops_.clear_ipv4(iface, ignored);
    }

    state = LinkState{};
    if (iface == eth_iface_) static_eth_ = StaticEthernetState{};

    std::string dns_error;
    if (!recompute_dns_locked(dns_error)) {
        error = dns_error;
        return false;
    }

    if (!ops_.start_dhcp(iface, error)) {
        return false;
    }
    state.managed_dhcp = true;
    return true;
}

void NetworkControlPlane::stop_dhcp(const std::string &iface) {
    std::lock_guard<std::mutex> guard(lock_);
    if (!known_iface(iface)) return;

    if (ops_.stop_dhcp) ops_.stop_dhcp(iface);
    if (ops_.clear_lease) ops_.clear_lease(iface);
    if (ops_.clear_default_route) ops_.clear_default_route(iface);
    if (ops_.clear_ipv4) {
        std::string ignored;
        (void)ops_.clear_ipv4(iface, ignored);
    }

    link_for(iface) = LinkState{};
    std::string ignored;
    (void)recompute_dns_locked(ignored);
}

bool NetworkControlPlane::apply_ethernet_static(const std::string &ip4,
                                                const std::string &netmask4,
                                                const std::string &gateway4,
                                                const std::string &dns4,
                                                int manual_metric,
                                                std::string &error) {
    std::lock_guard<std::mutex> guard(lock_);
    error.clear();
    if (!ops_.apply_ipv4 || !ops_.stop_dhcp || !ops_.clear_lease) {
        error = "static IPv4 platform ports are unavailable";
        return false;
    }

    ops_.stop_dhcp(eth_iface_);
    ops_.clear_lease(eth_iface_);
    if (ops_.clear_default_route) ops_.clear_default_route(eth_iface_);

    eth_ = LinkState{};
    static_eth_ = StaticEthernetState{};

    if (!ops_.apply_ipv4(eth_iface_, ip4, netmask4, error)) {
        return false;
    }

    static_eth_.active = true;
    static_eth_.gateway4 = gateway4;
    static_eth_.dns4 = dns4;
    static_eth_.manual_metric = manual_metric;

    if (!apply_routes_locked(error)) return false;
    return recompute_dns_locked(error);
}

bool NetworkControlPlane::reconcile_link_locked(const std::string &iface,
                                                LinkState &state,
                                                std::string &error) {
    if (!state.managed_dhcp) return true;
    if (!ops_.read_lease) {
        error = "DHCP lease reader is unavailable";
        return false;
    }

    DhcpLeaseFact fact;
    bool exists = false;
    if (!ops_.read_lease(iface, fact, exists, error)) return false;
    if (!exists) return true;

    const std::string fingerprint = fact.fingerprint();
    if (fingerprint == state.fingerprint) return true;

    if (fact.event == DhcpLeaseEvent::Deconfig) {
        if (ops_.clear_default_route) ops_.clear_default_route(iface);
        if (ops_.clear_ipv4) {
            std::string clear_error;
            if (!ops_.clear_ipv4(iface, clear_error)) {
                error = clear_error;
                return false;
            }
        }
        state.active = false;
        state.lease = fact;
        state.fingerprint = fingerprint;
        return true;
    }

    if (!fact.configured()) {
        error = "unsupported DHCP lease fact";
        return false;
    }
    if (!ops_.apply_ipv4) {
        error = "IPv4 configuration port is unavailable";
        return false;
    }
    if (!ops_.apply_ipv4(iface, fact.ip4, fact.netmask4, error)) return false;

    if (route_allowed(iface) && !fact.gateway4.empty()) {
        if (!ops_.set_default_route) {
            error = "default-route port is unavailable";
            return false;
        }
        if (!ops_.set_default_route(iface, fact.gateway4, route_metric(iface), error)) {
            return false;
        }
    } else if (ops_.clear_default_route) {
        ops_.clear_default_route(iface);
    }

    state.active = true;
    state.lease = fact;
    state.fingerprint = fingerprint;
    return true;
}

bool NetworkControlPlane::reconcile(std::string &error) {
    std::lock_guard<std::mutex> guard(lock_);
    error.clear();
    if (!reconcile_link_locked(eth_iface_, eth_, error)) return false;
    if (!reconcile_link_locked(wifi_iface_, wifi_, error)) return false;
    return recompute_dns_locked(error);
}

bool NetworkControlPlane::apply_routes_locked(std::string &error) {
    error.clear();

    if (static_eth_.active) {
        if (route_allowed(eth_iface_) && !static_eth_.gateway4.empty()) {
            if (!ops_.set_default_route ||
                !ops_.set_default_route(eth_iface_,
                                        static_eth_.gateway4,
                                        route_metric(eth_iface_, static_eth_.manual_metric),
                                        error)) {
                if (error.empty()) error = "failed to apply Ethernet static route";
                return false;
            }
        } else if (ops_.clear_default_route) {
            ops_.clear_default_route(eth_iface_);
        }
    } else if (eth_.active) {
        if (route_allowed(eth_iface_) && !eth_.lease.gateway4.empty()) {
            if (!ops_.set_default_route ||
                !ops_.set_default_route(eth_iface_,
                                        eth_.lease.gateway4,
                                        route_metric(eth_iface_),
                                        error)) {
                if (error.empty()) error = "failed to apply Ethernet DHCP route";
                return false;
            }
        } else if (ops_.clear_default_route) {
            ops_.clear_default_route(eth_iface_);
        }
    }

    if (wifi_.active) {
        if (route_allowed(wifi_iface_) && !wifi_.lease.gateway4.empty()) {
            if (!ops_.set_default_route ||
                !ops_.set_default_route(wifi_iface_,
                                        wifi_.lease.gateway4,
                                        route_metric(wifi_iface_),
                                        error)) {
                if (error.empty()) error = "failed to apply Wi-Fi DHCP route";
                return false;
            }
        } else if (ops_.clear_default_route) {
            ops_.clear_default_route(wifi_iface_);
        }
    }
    return true;
}

bool NetworkControlPlane::recompute_dns_locked(std::string &error) {
    error.clear();

    const bool static_eth_route = static_eth_.active &&
                                  route_allowed(eth_iface_) &&
                                  !static_eth_.gateway4.empty();
    const bool dhcp_eth_route = eth_.active &&
                                route_allowed(eth_iface_) &&
                                !eth_.lease.gateway4.empty();
    const bool wifi_route = wifi_.active &&
                            route_allowed(wifi_iface_) &&
                            !wifi_.lease.gateway4.empty();

    const std::string eth_dns = static_eth_.active ? static_eth_.dns4 : eth_.lease.dns4;
    const std::string wifi_dns = wifi_.lease.dns4;

    std::string selected;
    bool preserve_external = false;

    switch (route_policy_) {
    case RoutePolicy::WifiPreferred:
        if (wifi_route && !wifi_dns.empty()) selected = wifi_dns;
        else if ((static_eth_route || dhcp_eth_route) && !eth_dns.empty()) selected = eth_dns;
        break;
    case RoutePolicy::WifiOnly:
        if (wifi_route && !wifi_dns.empty()) selected = wifi_dns;
        break;
    case RoutePolicy::ManualMetric: {
        const int eth_metric = route_metric(eth_iface_, static_eth_.manual_metric);
        const int wifi_metric = route_metric(wifi_iface_);
        if (wifi_route &&
            ((!static_eth_route && !dhcp_eth_route) || wifi_metric < eth_metric)) {
            selected = wifi_dns;
        } else if ((static_eth_route || dhcp_eth_route) && !eth_dns.empty()) {
            selected = eth_dns;
        }
        break;
    }
    case RoutePolicy::EthernetPreferred:
    default:
        if ((static_eth_route || dhcp_eth_route) && !eth_dns.empty()) {
            selected = eth_dns;
        } else {
            if (ops_.snapshot) {
                const NetworkSnapshot live = ops_.snapshot();
                preserve_external = live.eth.has_default_route &&
                                    !static_eth_route && !dhcp_eth_route;
            }
            if (!preserve_external && wifi_route && !wifi_dns.empty()) {
                selected = wifi_dns;
            }
        }
        break;
    }

    if (preserve_external) return true;

    if (!selected.empty()) {
        if (managed_dns_ && selected == last_dns_) return true;
        if (!ops_.set_dns) {
            error = "DNS configuration port is unavailable";
            return false;
        }
        if (!ops_.set_dns(selected, error)) return false;
        managed_dns_ = true;
        last_dns_ = selected;
        return true;
    }

    if (!managed_dns_) return true;
    if (!ops_.clear_dns) {
        error = "DNS clear port is unavailable";
        return false;
    }
    if (!ops_.clear_dns(error)) return false;
    managed_dns_ = false;
    last_dns_.clear();
    return true;
}

bool NetworkControlPlane::set_route_policy(RoutePolicy policy, std::string &error) {
    std::lock_guard<std::mutex> guard(lock_);
    error.clear();
    const RoutePolicy previous = route_policy_;
    route_policy_ = policy;
    if (!apply_routes_locked(error) || !recompute_dns_locked(error)) {
        const std::string failure = error;
        route_policy_ = previous;
        std::string rollback_error;
        (void)apply_routes_locked(rollback_error);
        (void)recompute_dns_locked(rollback_error);
        error = failure;
        return false;
    }
    return true;
}

RoutePolicy NetworkControlPlane::route_policy() const {
    std::lock_guard<std::mutex> guard(lock_);
    return route_policy_;
}

} // namespace network_service
