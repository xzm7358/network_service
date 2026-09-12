#include "service/network_daemon.h"

#include "platform/wpa_event_monitor.h"
#include "service/network_control_plane.h"

namespace network_service {

bool NetworkDaemon::reconcile(bool &changed, std::string &error) {
    changed = false;
    if (!control_plane_) {
        error = "network control plane unavailable";
        return false;
    }

    // A production target without a usable kernel event source must retain the
    // pre-Netlink full 250 ms reconciliation semantics, including external
    // Ethernet/DNS policy refresh. Deterministic injected snapshots are kept
    // isolated from the host network and therefore use the lease-only path.
    if (!snapshot_provider_ && network_event_fd() < 0) {
        changed = true;
        return control_plane_->reconcile(error);
    }

    return control_plane_->reconcile(changed, error);
}

bool NetworkDaemon::consume_runtime_state_dirty() {
    // Deterministic injected snapshots and production systems without a usable
    // Netlink source retain the legacy 250 ms state-observation fallback.
    if (snapshot_provider_ || network_event_fd() < 0) return true;

    if (!wpa_monitor_) return false;
    const std::uint64_t current = wpa_monitor_->snapshot().event_sequence;
    const std::uint64_t previous = last_wpa_event_sequence_.exchange(current);
    return current != previous;
}

} // namespace network_service
