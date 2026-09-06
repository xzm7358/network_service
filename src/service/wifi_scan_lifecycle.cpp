#include "service/wifi_scan_lifecycle.h"

#include <utility>

namespace network_service {

WifiScanLifecycle::WifiScanLifecycle(StartFn start_fn,
                                     ResultsFn results_fn,
                                     EventsFn events_fn,
                                     ClockFn clock_fn,
                                     std::chrono::milliseconds timeout)
    : start_fn_(std::move(start_fn)),
      results_fn_(std::move(results_fn)),
      events_fn_(std::move(events_fn)),
      clock_fn_(std::move(clock_fn)),
      timeout_(timeout.count() > 0 ? timeout : std::chrono::milliseconds(5000)) {}

WifiScanLifecycle::Clock::time_point WifiScanLifecycle::now() const {
    return clock_fn_ ? clock_fn_() : Clock::now();
}

WifiScanEventMarkers WifiScanLifecycle::events() const {
    return events_fn_ ? events_fn_() : WifiScanEventMarkers{};
}

void WifiScanLifecycle::fail(std::string error) {
    state_ = WifiScanState::Failed;
    error_ = error.empty() ? "scan_failed" : std::move(error);
    records_.clear();
}

WifiScanStatus WifiScanLifecycle::start() {
    if (state_ == WifiScanState::Scanning) {
        return status();
    }

    ++scan_id_;
    if (scan_id_ == 0) ++scan_id_;
    state_ = WifiScanState::Scanning;
    error_.clear();
    records_.clear();
    baseline_ = events();
    deadline_ = now() + timeout_;

    std::string error;
    if (!start_fn_ || !start_fn_(error)) {
        fail(error.empty() ? "scan_start_failed" : error);
    }
    return status();
}

WifiScanStatus WifiScanLifecycle::poll() {
    if (state_ != WifiScanState::Scanning) {
        return status();
    }

    const WifiScanEventMarkers current = events();
    const bool current_scan_started = current.started_sequence > baseline_.sequence;
    if (current_scan_started &&
        current.failed_sequence > current.started_sequence &&
        current.failed_sequence > current.completed_sequence) {
        fail("scan_failed");
        return status();
    }

    if (current_scan_started && current.completed_sequence > current.started_sequence) {
        std::string error;
        std::vector<WifiApRecord> records = results_fn_ ? results_fn_(error) : std::vector<WifiApRecord>{};
        if (!error.empty()) {
            fail("scan_results_failed: " + error);
            return status();
        }
        records_ = std::move(records);
        state_ = WifiScanState::Ready;
        error_.clear();
        return status();
    }

    if (now() >= deadline_) {
        fail("timeout");
    }
    return status();
}

WifiScanStatus WifiScanLifecycle::status() const {
    WifiScanStatus out;
    out.scan_id = scan_id_;
    out.state = state_;
    out.error = error_;
    out.records = records_;
    return out;
}

const char *wifi_scan_state_name(WifiScanState state) {
    switch (state) {
    case WifiScanState::Idle: return "idle";
    case WifiScanState::Scanning: return "scanning";
    case WifiScanState::Ready: return "ready";
    case WifiScanState::Failed: return "failed";
    }
    return "failed";
}

} // namespace network_service
