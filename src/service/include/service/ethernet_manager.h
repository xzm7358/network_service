#ifndef NETWORK_SERVICE_ETHERNET_MANAGER_H
#define NETWORK_SERVICE_ETHERNET_MANAGER_H

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

#include "network_service_types.h"

namespace network_service {

struct EthernetManagerState {
    bool dhcp_enabled = false;
    bool static_enabled = false;
    bool static_runtime_active = false;
    bool static_restore_pending = false;
    bool carrier_up = false;
    DhcpClientState dhcp_state = DhcpClientState::Idle;
    std::uint32_t dhcp_requests = 0;
    std::uint32_t retry_attempts = 0;
    bool retry_scheduled = false;
    std::string failure_reason;
};

class EthernetManager {
public:
    using Clock = std::chrono::steady_clock;
    using DhcpStartFn = std::function<bool(std::string &error)>;
    using DhcpStopFn = std::function<void()>;
    using DhcpRunningFn = std::function<bool()>;
    using StaticApplyFn = std::function<bool(std::string &error)>;
    using NowFn = std::function<Clock::time_point()>;

    EthernetManager(
        DhcpStartFn dhcp_start,
        DhcpStopFn dhcp_stop,
        DhcpRunningFn dhcp_running,
        NowFn now = {},
        std::chrono::milliseconds initial_retry = std::chrono::seconds(1),
        std::chrono::milliseconds maximum_retry = std::chrono::seconds(8),
        std::uint32_t maximum_retry_attempts = 5);

    // Explicit/startup DHCP activation performs one immediate attempt. A failed
    // attempt enters the same bounded retry lifecycle used for process exits.
    bool start_dhcp_now(std::string &error);
    void adopt_dhcp_running();

    // Called only after a static runtime transaction succeeds. The control plane
    // has already stopped DHCP and replaced its owned IP/route/DNS state.
    void set_static_mode(StaticApplyFn static_apply = {});

    // Reconciles carrier and DHCP process truth. Returns true when lifecycle
    // state changed; retry deadlines are evaluated against the injected clock.
    bool reconcile(bool carrier_up);

    EthernetManagerState state() const;

private:
    std::chrono::milliseconds retry_delay(std::uint32_t completed_retries) const;
    void schedule_retry_locked();
    bool run_start_locked(bool retry, std::string &error);

    DhcpStartFn dhcp_start_;
    DhcpStopFn dhcp_stop_;
    DhcpRunningFn dhcp_running_;
    StaticApplyFn static_apply_;
    NowFn now_;
    std::chrono::milliseconds initial_retry_;
    std::chrono::milliseconds maximum_retry_;
    std::uint32_t maximum_retry_attempts_;

    mutable std::mutex lock_;
    EthernetManagerState state_;
    Clock::time_point retry_at_{};

    // All Platform operations are serialized independently from state reads.
    mutable std::mutex operation_lock_;
};

} // namespace network_service

#endif // NETWORK_SERVICE_ETHERNET_MANAGER_H
