#include "service/wifi_manager.h"

#include <utility>

namespace network_service {

WifiManager::WifiManager(DhcpStartFn dhcp_start, DhcpStopFn dhcp_stop)
    : dhcp_start_(std::move(dhcp_start)),
      dhcp_stop_(std::move(dhcp_stop)) {}

void WifiManager::on_l2_connected() {
    std::lock_guard<std::mutex> guard(lock_);
    state_.l2_connected = true;
    state_.failure_reason.clear();

    if (state_.dhcp_requested) {
        return;
    }

    ++state_.dhcp_requests;
    std::string error;
    if (!dhcp_start_ || !dhcp_start_(error)) {
        state_.dhcp_requested = false;
        state_.failure_reason = "dhcp_start_failed";
        if (!error.empty()) {
            state_.failure_reason += ": " + error;
        }
        return;
    }

    state_.dhcp_requested = true;
}

void WifiManager::on_l2_disconnected() {
    std::lock_guard<std::mutex> guard(lock_);
    if (state_.dhcp_requested && dhcp_stop_) {
        dhcp_stop_();
    }
    state_.l2_connected = false;
    state_.dhcp_requested = false;
    state_.failure_reason.clear();
}

void WifiManager::stop_dhcp() {
    std::lock_guard<std::mutex> guard(lock_);
    if (state_.dhcp_requested && dhcp_stop_) {
        dhcp_stop_();
    }
    state_.dhcp_requested = false;
}

WifiManagerState WifiManager::state() const {
    std::lock_guard<std::mutex> guard(lock_);
    return state_;
}

} // namespace network_service
