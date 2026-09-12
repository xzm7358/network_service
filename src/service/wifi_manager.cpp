#include "service/wifi_manager.h"

#include <utility>

namespace network_service {

WifiManager::WifiManager(DhcpStartFn dhcp_start, DhcpStopFn dhcp_stop)
    : dhcp_start_(std::move(dhcp_start)),
      dhcp_stop_(std::move(dhcp_stop)) {}

void WifiManager::on_l2_connected() {
    {
        std::lock_guard<std::mutex> guard(lock_);
        state_.l2_connected = true;
        state_.failure_reason.clear();
        if (state_.dhcp_requested) return;

        // Reserve the lifecycle transition before releasing the state lock so
        // concurrent CONNECTED events cannot schedule another DHCP start.
        state_.dhcp_requested = true;
        ++state_.dhcp_requests;
    }

    std::lock_guard<std::mutex> operation_guard(operation_lock_);

    // The link may have disconnected while this operation was waiting to run.
    {
        std::lock_guard<std::mutex> guard(lock_);
        if (!state_.l2_connected) {
            state_.dhcp_requested = false;
            return;
        }
    }

    std::string error;
    const bool started = dhcp_start_ && dhcp_start_(error);
    bool stop_after_start = false;
    {
        std::lock_guard<std::mutex> guard(lock_);
        if (!started) {
            state_.dhcp_requested = false;
            state_.failure_reason = "dhcp_start_failed";
            if (!error.empty()) {
                state_.failure_reason += ": " + error;
            }
            return;
        }

        // DISCONNECTED may have arrived while the blocking platform start was
        // in progress. Do not publish a live DHCP lifecycle for a dead L2 link.
        if (!state_.l2_connected) {
            state_.dhcp_requested = false;
            stop_after_start = true;
        }
    }

    if (stop_after_start && dhcp_stop_) {
        dhcp_stop_();
    }
}

void WifiManager::on_l2_disconnected() {
    bool should_stop = false;
    {
        std::lock_guard<std::mutex> guard(lock_);
        should_stop = state_.dhcp_requested;
        state_.l2_connected = false;
        state_.dhcp_requested = false;
        state_.failure_reason.clear();
    }

    if (should_stop && dhcp_stop_) {
        std::lock_guard<std::mutex> operation_guard(operation_lock_);
        dhcp_stop_();
    }
}

void WifiManager::stop_dhcp() {
    bool should_stop = false;
    {
        std::lock_guard<std::mutex> guard(lock_);
        should_stop = state_.dhcp_requested;
        state_.dhcp_requested = false;
    }

    if (should_stop && dhcp_stop_) {
        std::lock_guard<std::mutex> operation_guard(operation_lock_);
        dhcp_stop_();
    }
}

WifiManagerState WifiManager::state() const {
    std::lock_guard<std::mutex> guard(lock_);
    return state_;
}

} // namespace network_service
