#include "service/network_control_plane.h"

namespace network_service {

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
