#include "service/network_control_plane.h"

namespace network_service {

bool NetworkControlPlane::adopt_dhcp(const std::string &iface, std::string &error) {
    std::lock_guard<std::mutex> guard(lock_);
    error.clear();
    if (!known_iface(iface)) {
        error = "unknown DHCP interface";
        return false;
    }
    if (!ops_.read_lease) {
        error = "DHCP lease reader is unavailable";
        return false;
    }

    LinkState &state = link_for(iface);
    state = LinkState{};
    state.managed_dhcp = true;
    if (iface == eth_iface_) {
        // A verified running DHCP client is authoritative evidence that the
        // interface is not currently owned by our static-Ethernet lifecycle.
        static_eth_ = StaticEthernetState{};
    }

    // Deliberately do not apply or clear anything here. The normal reconcile
    // path consumes the current generation's lease fact and converges owned
    // state from that fact. If no lease exists yet, adoption simply waits for
    // the already-running client to publish one.
    return true;
}

bool NetworkControlPlane::repair_owned_state_locked(std::string &error) {
    error.clear();
    if (!ops_.snapshot) return true;

    const NetworkSnapshot live = ops_.snapshot();

    auto repair_ipv4 = [&](const std::string &iface,
                           const InterfaceSnapshot &observed,
                           const std::string &ip4,
                           const std::string &netmask4) -> bool {
        if (!observed.exists || ip4.empty() || netmask4.empty()) return true;
        if (observed.has_ip && observed.ip4 == ip4 && observed.netmask4 == netmask4) {
            return true;
        }
        if (!ops_.apply_ipv4) {
            error = "IPv4 repair port is unavailable";
            return false;
        }
        return ops_.apply_ipv4(iface, ip4, netmask4, error);
    };

    auto repair_missing_route = [&](const std::string &iface,
                                    const InterfaceSnapshot &observed,
                                    const std::string &gateway4,
                                    int metric) -> bool {
        if (!observed.exists || !route_allowed(iface) || gateway4.empty()) return true;
        if (observed.has_default_route) {
            // Until route ownership identity is introduced, never replace a live
            // route merely because its gateway/metric differs. It may belong to
            // another manager. F-04 repairs only a route that disappeared.
            return true;
        }
        if (!ops_.set_default_route) {
            error = "default-route repair port is unavailable";
            return false;
        }
        return ops_.set_default_route(iface, gateway4, metric, error);
    };

    if (static_eth_.active) {
        if (!repair_ipv4(eth_iface_,
                         live.eth,
                         static_eth_.ip4,
                         static_eth_.netmask4)) {
            return false;
        }
        if (!repair_missing_route(eth_iface_,
                                  live.eth,
                                  static_eth_.gateway4,
                                  route_metric(eth_iface_, static_eth_.manual_metric))) {
            return false;
        }
    } else if (eth_.active && eth_.lease.configured()) {
        if (!repair_ipv4(eth_iface_, live.eth, eth_.lease.ip4, eth_.lease.netmask4)) {
            return false;
        }
        if (!repair_missing_route(eth_iface_,
                                  live.eth,
                                  eth_.lease.gateway4,
                                  route_metric(eth_iface_))) {
            return false;
        }
    }

    if (wifi_.active && wifi_.lease.configured()) {
        if (!repair_ipv4(wifi_iface_, live.wifi, wifi_.lease.ip4, wifi_.lease.netmask4)) {
            return false;
        }
        if (!repair_missing_route(wifi_iface_,
                                  live.wifi,
                                  wifi_.lease.gateway4,
                                  route_metric(wifi_iface_))) {
            return false;
        }
    }

    return true;
}

bool NetworkControlPlane::reconcile(bool &changed, std::string &error) {
    std::lock_guard<std::mutex> guard(lock_);
    error.clear();
    changed = false;

    bool eth_changed = false;
    bool wifi_changed = false;
    if (!reconcile_link_locked(eth_iface_, eth_, eth_changed, error)) return false;
    if (!reconcile_link_locked(wifi_iface_, wifi_, wifi_changed, error)) return false;

    changed = eth_changed || wifi_changed;
    if (!changed) return true;
    return recompute_dns_locked(error);
}

} // namespace network_service
