#include "service/ethernet_manager.h"

#include <algorithm>
#include <utility>

namespace network_service {

EthernetManager::EthernetManager(
    DhcpStartFn dhcp_start,
    DhcpStopFn dhcp_stop,
    DhcpRunningFn dhcp_running,
    NowFn now,
    std::chrono::milliseconds initial_retry,
    std::chrono::milliseconds maximum_retry,
    std::uint32_t maximum_retry_attempts)
    : dhcp_start_(std::move(dhcp_start)),
      dhcp_stop_(std::move(dhcp_stop)),
      dhcp_running_(std::move(dhcp_running)),
      now_(std::move(now)),
      initial_retry_(std::max(initial_retry, std::chrono::milliseconds(1))),
      maximum_retry_(std::max(maximum_retry, initial_retry_)),
      maximum_retry_attempts_(maximum_retry_attempts) {
    if (!now_) now_ = []() { return Clock::now(); };
}

std::chrono::milliseconds EthernetManager::retry_delay(
    std::uint32_t completed_retries) const {
    std::chrono::milliseconds delay = initial_retry_;
    for (std::uint32_t i = 0; i < completed_retries; ++i) {
        if (delay >= maximum_retry_ / 2) return maximum_retry_;
        delay *= 2;
    }
    return std::min(delay, maximum_retry_);
}

void EthernetManager::schedule_retry_locked() {
    if (!state_.dhcp_enabled || !state_.carrier_up ||
        state_.retry_attempts >= maximum_retry_attempts_) {
        state_.retry_scheduled = false;
        return;
    }
    retry_at_ = now_() + retry_delay(state_.retry_attempts);
    state_.retry_scheduled = true;
}

bool EthernetManager::run_start_locked(bool retry, std::string &error) {
    {
        std::lock_guard<std::mutex> guard(lock_);
        state_.dhcp_state = DhcpClientState::Starting;
        state_.retry_scheduled = false;
        state_.failure_reason.clear();
        ++state_.dhcp_requests;
        if (retry) ++state_.retry_attempts;
    }

    error.clear();
    const bool started = dhcp_start_ && dhcp_start_(error);
    std::lock_guard<std::mutex> guard(lock_);
    if (started) {
        state_.dhcp_state = DhcpClientState::Running;
        state_.retry_scheduled = false;
        state_.failure_reason.clear();
        return true;
    }

    state_.dhcp_state = DhcpClientState::Failed;
    state_.failure_reason = "dhcp_start_failed";
    if (!error.empty()) state_.failure_reason += ": " + error;
    schedule_retry_locked();
    return false;
}

bool EthernetManager::start_dhcp_now(std::string &error) {
    std::lock_guard<std::mutex> operation_guard(operation_lock_);
    {
        std::lock_guard<std::mutex> guard(lock_);
        state_.dhcp_enabled = true;
        state_.static_enabled = false;
        state_.static_runtime_active = false;
        state_.static_restore_pending = false;
        static_apply_ = {};
        // Startup precedes the first carrier observation. Permit retry
        // scheduling until authoritative carrier truth arrives.
        state_.carrier_up = true;
        state_.retry_attempts = 0;
        state_.retry_scheduled = false;
    }
    return run_start_locked(false, error);
}

void EthernetManager::adopt_dhcp_running() {
    std::lock_guard<std::mutex> operation_guard(operation_lock_);
    std::lock_guard<std::mutex> guard(lock_);
    state_.dhcp_enabled = true;
    state_.static_enabled = false;
    state_.static_runtime_active = false;
    state_.static_restore_pending = false;
    static_apply_ = {};
    state_.carrier_up = true;
    state_.dhcp_state = DhcpClientState::Running;
    state_.retry_attempts = 0;
    state_.retry_scheduled = false;
    state_.failure_reason.clear();
}

void EthernetManager::set_static_mode(StaticApplyFn static_apply) {
    std::lock_guard<std::mutex> operation_guard(operation_lock_);
    std::lock_guard<std::mutex> guard(lock_);
    state_.dhcp_enabled = false;
    state_.static_enabled = true;
    state_.static_runtime_active = true;
    state_.static_restore_pending = false;
    static_apply_ = std::move(static_apply);
    state_.dhcp_state = DhcpClientState::Idle;
    state_.retry_attempts = 0;
    state_.retry_scheduled = false;
    state_.failure_reason.clear();
}

bool EthernetManager::reconcile(bool carrier_up) {
    std::lock_guard<std::mutex> operation_guard(operation_lock_);

    bool changed = false;
    bool inspect_running = false;
    bool start = false;
    bool retry = false;
    bool stop = false;
    bool restore_static = false;
    {
        std::lock_guard<std::mutex> guard(lock_);
        changed = state_.carrier_up != carrier_up;
        state_.carrier_up = carrier_up;
        if (state_.static_enabled) {
            if (!carrier_up) {
                stop = state_.static_runtime_active;
                state_.static_runtime_active = false;
                state_.static_restore_pending = true;
                state_.failure_reason.clear();
                changed = changed || stop;
            } else if (state_.static_restore_pending) {
                state_.static_restore_pending = false;
                restore_static = true;
            }
        } else if (!state_.dhcp_enabled) {
            return changed;
        }

        if (state_.static_enabled) {
            // Static restore/clear actions are executed below without state_lock.
        } else if (!carrier_up) {
            stop = state_.dhcp_state != DhcpClientState::Idle ||
                   state_.retry_scheduled;
            state_.dhcp_state = DhcpClientState::Idle;
            state_.retry_attempts = 0;
            state_.retry_scheduled = false;
            state_.failure_reason.clear();
            changed = changed || stop;
        } else if (state_.dhcp_state == DhcpClientState::Idle) {
            start = true;
        } else if (state_.dhcp_state == DhcpClientState::Running) {
            inspect_running = true;
        } else if (state_.dhcp_state == DhcpClientState::Failed &&
                   state_.retry_scheduled && now_() >= retry_at_) {
            start = true;
            retry = true;
        }
    }

    if (stop) {
        if (dhcp_stop_) dhcp_stop_();
        return true;
    }
    if (restore_static) {
        std::string error;
        const bool applied = static_apply_ && static_apply_(error);
        std::lock_guard<std::mutex> guard(lock_);
        state_.static_runtime_active = applied;
        state_.failure_reason = applied ? "" : "static_restore_failed";
        if (!applied && !error.empty()) state_.failure_reason += ": " + error;
        return true;
    }
    if (start) {
        std::string ignored;
        (void)run_start_locked(retry, ignored);
        return true;
    }
    if (!inspect_running || !dhcp_running_ || dhcp_running_()) return changed;

    std::lock_guard<std::mutex> guard(lock_);
    if (!state_.dhcp_enabled || !state_.carrier_up ||
        state_.dhcp_state != DhcpClientState::Running) {
        return changed;
    }
    state_.dhcp_state = DhcpClientState::Failed;
    state_.retry_attempts = 0;
    state_.failure_reason = "dhcp_process_exited";
    schedule_retry_locked();
    return true;
}

EthernetManagerState EthernetManager::state() const {
    std::lock_guard<std::mutex> guard(lock_);
    return state_;
}

} // namespace network_service
