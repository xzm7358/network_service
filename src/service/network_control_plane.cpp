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

NetworkControlPlane::OwnedRouteState &NetworkControlPlane::owned_route_for(
    const std::string &iface) {
    return iface == eth_iface_ ? eth_route_ : wifi_route_;
}

bool NetworkControlPlane::clear_owned_route_locked(const std::string &iface,
                                                   std::string &error) {
    error.clear();
    OwnedRouteState &owned = owned_route_for(iface);
    if (!owned.active) return true;
    if (!ops_.clear_default_route) {
        error = "default-route clear port is unavailable";
        return false;
    }
    if (!ops_.clear_default_route(iface, owned.gateway4, owned.metric, error)) {
        return false;
    }
    owned = OwnedRouteState{};
    return true;
}

bool NetworkControlPlane::ensure_owned_route_locked(const std::string &iface,
                                                    const std::string &gateway4,
                                                    int metric,
                                                    std::string &error) {
    error.clear();
    if (gateway4.empty()) return clear_owned_route_locked(iface, error);
    if (!ops_.set_default_route) {
        error = "default-route port is unavailable";
        return false;
    }

    OwnedRouteState &owned = owned_route_for(iface);
    if (owned.active && (owned.gateway4 != gateway4 || owned.metric != metric)) {
        if (!clear_owned_route_locked(iface, error)) return false;
    }

    // Platform implements this as ensure-exact: if the route already exists
    // (for example after daemon Brownfield restart) this is a non-mutating
    // ownership adoption; otherwise it creates only this exact identity.
    if (!ops_.set_default_route(iface, gateway4, metric, error)) return false;
    owned.active = true;
    owned.gateway4 = gateway4;
    owned.metric = metric;
    return true;
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

    if (!clear_owned_route_locked(iface, error)) return false;

    LinkState &state = link_for(iface);
    ops_.stop_dhcp(iface);
    ops_.clear_lease(iface);
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

    if (!ops_.start_dhcp(iface, error)) return false;
    state.managed_dhcp = true;
    return true;
}

void NetworkControlPlane::stop_dhcp(const std::string &iface) {
    std::lock_guard<std::mutex> guard(lock_);
    if (!known_iface(iface)) return;

    std::string ignored;
    (void)clear_owned_route_locked(iface, ignored);
    if (ops_.stop_dhcp) ops_.stop_dhcp(iface);
    if (ops_.clear_lease) ops_.clear_lease(iface);
    if (ops_.clear_ipv4) (void)ops_.clear_ipv4(iface, ignored);

    link_for(iface) = LinkState{};
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

    if (!clear_owned_route_locked(eth_iface_, error)) return false;
    ops_.stop_dhcp(eth_iface_);
    ops_.clear_lease(eth_iface_);

    eth_ = LinkState{};
    static_eth_ = StaticEthernetState{};

    if (!ops_.apply_ipv4(eth_iface_, ip4, netmask4, error)) return false;

    static_eth_.active = true;
    static_eth_.ip4 = ip4;
    static_eth_.netmask4 = netmask4;
    static_eth_.gateway4 = gateway4;
    static_eth_.dns4 = dns4;
    static_eth_.manual_metric = manual_metric;

    if (!apply_routes_locked(error)) return false;
    return recompute_dns_locked(error);
}

bool NetworkControlPlane::reconcile_link_locked(const std::string &iface,
                                                LinkState &state,
                                                bool &changed,
                                                std::string &error) {
    changed = false;
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
        if (!clear_owned_route_locked(iface, error)) return false;
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
        changed = true;
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
        if (!ensure_owned_route_locked(iface, fact.gateway4, route_metric(iface), error)) {
            return false;
        }
    } else if (!clear_owned_route_locked(iface, error)) {
        return false;
    }

    state.active = true;
    state.lease = fact;
    state.fingerprint = fingerprint;
    changed = true;
    return true;
}

bool NetworkControlPlane::reconcile(std::string &error) {
    std::lock_guard<std::mutex> guard(lock_);
    error.clear();
    bool eth_changed = false;
    bool wifi_changed = false;
    if (!reconcile_link_locked(eth_iface_, eth_, eth_changed, error)) return false;
    if (!reconcile_link_locked(wifi_iface_, wifi_, wifi_changed, error)) return false;

    // Full reconciliation is used when Netlink is unavailable. In that mode it
    // must both consume new lease facts and repair drift in state we already own.
    if (!repair_owned_state_locked(error)) return false;
    return recompute_dns_locked(error);
}

bool NetworkControlPlane::refresh_external_state(std::string &error) {
    std::lock_guard<std::mutex> guard(lock_);
    error.clear();
    // Netlink is an observation trigger, not an owner. Read authoritative state,
    // repair only resources for which Service already has explicit desired facts,
    // then recompute DNS/readiness policy.
    if (!repair_owned_state_locked(error)) return false;
    return recompute_dns_locked(error);
}

bool NetworkControlPlane::apply_routes_locked(std::string &error) {
    error.clear();

    if (static_eth_.active && route_allowed(eth_iface_) && !static_eth_.gateway4.empty()) {
        if (!ensure_owned_route_locked(eth_iface_,
                                       static_eth_.gateway4,
                                       route_metric(eth_iface_, static_eth_.manual_metric),
                                       error)) {
            if (error.empty()) error = "failed to apply Ethernet static route";
            return false;
        }
    } else if (eth_.active && route_allowed(eth_iface_) && !eth_.lease.gateway4.empty()) {
        if (!ensure_owned_route_locked(eth_iface_,
                                       eth_.lease.gateway4,
                                       route_metric(eth_iface_),
                                       error)) {
            if (error.empty()) error = "failed to apply Ethernet DHCP route";
            return false;
        }
    } else if (!clear_owned_route_locked(eth_iface_, error)) {
        return false;
    }

    if (wifi_.active && route_allowed(wifi_iface_) && !wifi_.lease.gateway4.empty()) {
        if (!ensure_owned_route_locked(wifi_iface_,
                                       wifi_.lease.gateway4,
                                       route_metric(wifi_iface_),
                                       error)) {
            if (error.empty()) error = "failed to apply Wi-Fi DHCP route";
            return false;
        }
    } else if (!clear_owned_route_locked(wifi_iface_, error)) {
        return false;
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
    NetworkSnapshot live;
    bool have_live = false;
    auto load_live = [&]() -> const NetworkSnapshot & {
        if (!have_live && ops_.snapshot) {
            live = ops_.snapshot();
            have_live = true;
        }
        return live;
    };

    // Recover only historical ownership identity here. The marker is a Platform
    // fact, not a policy decision: the switch below still decides whether this
    // process should keep, update, or relinquish the resolver under current route
    // truth. Marker absence means any existing resolver belongs to someone else.
    if (!dns_ownership_initialized_) {
        if (ops_.snapshot) {
            const NetworkSnapshot &current = load_live();
            if (current.dns_managed_by_network_service) {
                managed_dns_ = true;
                last_dns_ = current.dns4;
            } else {
                managed_dns_ = false;
                last_dns_.clear();
            }
        }
        dns_ownership_initialized_ = true;
    }

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
                const NetworkSnapshot &current = load_live();
                preserve_external = current.eth.has_default_route &&
                                    !static_eth_route && !dhcp_eth_route;
            }
            if (!preserve_external && wifi_route && !wifi_dns.empty()) {
                selected = wifi_dns;
            }
        }
        break;
    }

    if (preserve_external) {
        if (managed_dns_) {
            if (!ops_.clear_dns) {
                error = "DNS clear port is unavailable";
                return false;
            }
            if (!ops_.clear_dns(error)) return false;
            managed_dns_ = false;
            last_dns_.clear();
        }
        return true;
    }

    if (!selected.empty()) {
        if (managed_dns_ && selected == last_dns_) {
            if (!ops_.snapshot || load_live().dns4 == selected) return true;
        }
        if (!ops_.set_dns) {
            error = "DNS configuration port is unavailable";
            return false;
        }
        if (!ops_.set_dns(selected, error)) return false;
        managed_dns_ = true;
        last_dns_.clear();
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
