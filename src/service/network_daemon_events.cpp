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
    return control_plane_->reconcile(changed, error);
}

bool NetworkDaemon::consume_runtime_state_dirty() {
    if (!wpa_monitor_) return false;
    const std::uint64_t current = wpa_monitor_->snapshot().event_sequence;
    const std::uint64_t previous = last_wpa_event_sequence_.exchange(current);
    return current != previous;
}

} // namespace network_service
