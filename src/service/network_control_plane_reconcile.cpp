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
