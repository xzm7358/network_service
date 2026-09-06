#ifndef NETWORK_SERVICE_WIFI_SCAN_LIFECYCLE_H
#define NETWORK_SERVICE_WIFI_SCAN_LIFECYCLE_H

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "platform/wifi_backend.h"

namespace network_service {

enum class WifiScanState {
    Idle,
    Scanning,
    Ready,
    Failed,
};

struct WifiScanEventMarkers {
    std::uint64_t sequence = 0;
    std::uint64_t started_sequence = 0;
    std::uint64_t completed_sequence = 0;
    std::uint64_t failed_sequence = 0;
};

struct WifiScanStatus {
    std::uint64_t scan_id = 0;
    WifiScanState state = WifiScanState::Idle;
    std::string error;
    std::vector<WifiApRecord> records;
};

class WifiScanLifecycle {
public:
    using StartFn = std::function<bool(std::string &)>;
    using ResultsFn = std::function<std::vector<WifiApRecord>(std::string &)>;
    using EventsFn = std::function<WifiScanEventMarkers()>;
    using Clock = std::chrono::steady_clock;
    using ClockFn = std::function<Clock::time_point()>;

    WifiScanLifecycle(StartFn start_fn,
                      ResultsFn results_fn,
                      EventsFn events_fn,
                      ClockFn clock_fn = {},
                      std::chrono::milliseconds timeout = std::chrono::milliseconds(5000));

    WifiScanStatus start();
    WifiScanStatus poll();
    WifiScanStatus status() const;

private:
    Clock::time_point now() const;
    WifiScanEventMarkers events() const;
    void fail(std::string error);

    StartFn start_fn_;
    ResultsFn results_fn_;
    EventsFn events_fn_;
    ClockFn clock_fn_;
    std::chrono::milliseconds timeout_;

    std::uint64_t scan_id_ = 0;
    WifiScanState state_ = WifiScanState::Idle;
    std::string error_;
    std::vector<WifiApRecord> records_;
    WifiScanEventMarkers baseline_;
    Clock::time_point deadline_{};
};

const char *wifi_scan_state_name(WifiScanState state);

} // namespace network_service

#endif // NETWORK_SERVICE_WIFI_SCAN_LIFECYCLE_H
