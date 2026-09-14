#include "service/network_daemon.h"

#include "platform/wpa_event_monitor.h"
#include "service/network_control_plane.h"
#include "service/wifi_manager.h"

namespace network_service {

bool NetworkDaemon::reconcile(bool &changed, std::string &error) {
    changed = false;
    if (!control_plane_) {
        error = "network control plane unavailable";
        return false;
    }

    const bool ethernet_lifecycle_changed = reconcile_ethernet_lifecycle();
    const bool wifi_dhcp_process_changed =
        wifi_manager_ && wifi_manager_->reconcile_dhcp_process();

    // A production target without a usable kernel event source must retain the
    // pre-Netlink full 250 ms reconciliation semantics, including external
    // Ethernet/DNS policy refresh. Deterministic injected snapshots are kept
    // isolated from the host network and therefore use the lease-only path.
    if (!snapshot_provider_ && network_event_fd() < 0) {
        changed = true;
        return control_plane_->reconcile(error);
    }

    bool network_changed = false;
    const bool ok = control_plane_->reconcile(network_changed, error);
    changed = ethernet_lifecycle_changed || wifi_dhcp_process_changed || network_changed;
    return ok;
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

NetworkOperationResult<RoutePolicyInfo> NetworkDaemon::route_policy_get() const {
    if (!control_plane_) {
        return NetworkOperationResult<RoutePolicyInfo>::failure(
            500, "network control plane unavailable");
    }
    RoutePolicyInfo info;
    info.policy = control_plane_->route_policy();
    info.persistent = false;
    return NetworkOperationResult<RoutePolicyInfo>::success(info);
}

NetworkOperationResult<RoutePolicyInfo> NetworkDaemon::route_policy_apply(
    RoutePolicy policy) {
    if (!control_plane_) {
        return NetworkOperationResult<RoutePolicyInfo>::failure(
            500, "network control plane unavailable");
    }

    // ManualMetric exists as an internal control-plane mode but has no complete
    // product parameter contract yet (notably no Wi-Fi metric input). Do not let
    // callers reach a partially specified policy through this Service surface.
    if (policy == RoutePolicy::ManualMetric) {
        return NetworkOperationResult<RoutePolicyInfo>::failure(
            400, "manual_metric is not a supported product route policy");
    }

    std::string error;
    if (!control_plane_->set_route_policy(policy, error)) {
        return NetworkOperationResult<RoutePolicyInfo>::failure(
            500, error.empty() ? "failed to apply route policy" : std::move(error));
    }

    RoutePolicyInfo info;
    info.policy = control_plane_->route_policy();
    info.persistent = false;
    return NetworkOperationResult<RoutePolicyInfo>::success(info);
}

} // namespace network_service
