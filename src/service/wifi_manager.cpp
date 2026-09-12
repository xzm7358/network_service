#include "service/wifi_manager.h"

#include <utility>

namespace network_service {
namespace {

bool dhcp_state_active(DhcpClientState state) {
    return state == DhcpClientState::Starting || state == DhcpClientState::Running;
}

} // namespace

WifiManager::WifiManager(DhcpStartFn dhcp_start,
                         DhcpStopFn dhcp_stop,
                         DhcpRunningFn dhcp_running)
    : dhcp_start_(std::move(dhcp_start)),
      dhcp_stop_(std::move(dhcp_stop)),
      dhcp_running_(std::move(dhcp_running)) {}

void WifiManager::on_l2_connected() {
    {
        std::lock_guard<std::mutex> guard(lock_);
        state_.l2_connected = true;
        state_.failure_reason.clear();
        if (dhcp_state_active(state_.dhcp_state)) return;

        // Reserve the lifecycle transition before releasing the state lock so
        // concurrent CONNECTED events cannot schedule another DHCP start.
        state_.dhcp_state = DhcpClientState::Starting;
        ++state_.dhcp_requests;
    }

    std::lock_guard<std::mutex> operation_guard(operation_lock_);

    // The link may have disconnected while this operation was waiting to run.
    {
        std::lock_guard<std::mutex> guard(lock_);
        if (!state_.l2_connected) {
            state_.dhcp_state = DhcpClientState::Idle;
            return;
        }
    }

    std::string error;
    const bool started = dhcp_start_ && dhcp_start_(error);
    {
        std::lock_guard<std::mutex> guard(lock_);
        if (!started) {
            state_.dhcp_state = DhcpClientState::Failed;
            state_.failure_reason = "dhcp_start_failed";
            if (!error.empty()) {
                state_.failure_reason += ": " + error;
            }
            return;
        }

        // If DISCONNECTED/explicit stop arrived while start was in progress,
        // that cancelling transition already owns the serialized stop operation.
        // Do not issue a second stop from the start path.
        if (!state_.l2_connected) {
            state_.dhcp_state = DhcpClientState::Idle;
            return;
        }
        state_.dhcp_state = DhcpClientState::Running;
    }
}

void WifiManager::on_l2_disconnected() {
    bool should_stop = false;
    {
        std::lock_guard<std::mutex> guard(lock_);
        should_stop = dhcp_state_active(state_.dhcp_state);
        state_.l2_connected = false;
        state_.dhcp_state = DhcpClientState::Idle;
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
        should_stop = dhcp_state_active(state_.dhcp_state);
        state_.dhcp_state = DhcpClientState::Idle;
        state_.failure_reason.clear();
    }

    if (should_stop && dhcp_stop_) {
        std::lock_guard<std::mutex> operation_guard(operation_lock_);
        dhcp_stop_();
    }
}

void WifiManager::adopt_dhcp_running() {
    std::lock_guard<std::mutex> operation_guard(operation_lock_);
    std::lock_guard<std::mutex> guard(lock_);
    // Platform has already verified process identity. Do not mark L2 connected:
    // an existing DHCP client and current supplicant L2 state are independent
    // facts and the latter is reconciled by the WPA path.
    state_.dhcp_state = DhcpClientState::Running;
    state_.failure_reason.clear();
}

bool WifiManager::reconcile_dhcp_process() {
    {
        std::lock_guard<std::mutex> guard(lock_);
        if (state_.dhcp_state != DhcpClientState::Running) return false;
    }
    if (!dhcp_running_) return false;

    std::lock_guard<std::mutex> operation_guard(operation_lock_);
    const bool running = dhcp_running_();
    if (running) return false;

    std::lock_guard<std::mutex> guard(lock_);
    // A disconnect/explicit stop may have won while process inspection waited
    // for the serialized mechanism lock. Never resurrect a cancelled lifecycle.
    if (state_.dhcp_state != DhcpClientState::Running) return false;
    state_.dhcp_state = DhcpClientState::Failed;
    state_.failure_reason = "dhcp_process_exited";
    return true;
}

WifiManagerState WifiManager::state() const {
    std::lock_guard<std::mutex> guard(lock_);
    return state_;
}

} // namespace network_service
