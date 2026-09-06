#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SELF = ROOT / "tools/ns_rc_001_generation_fence.py"
WORKFLOW = ROOT / ".github/workflows/ns-rc-001-generation-fence.yml"


def replace_once(rel: str, old: str, new: str) -> None:
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{rel}: expected one match, found {count}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8")


replace_once(
    "src/platform/wpa_event_monitor.h",
    "    uint32_t scan_failed_events = 0;\n",
    "    uint32_t scan_failed_events = 0;\n"
    "    uint64_t event_sequence = 0;\n"
    "    uint64_t last_scan_started_sequence = 0;\n"
    "    uint64_t last_scan_result_sequence = 0;\n"
    "    uint64_t last_scan_failed_sequence = 0;\n",
)

replace_once(
    "src/platform/wpa_event_monitor.cpp",
    '''    snapshot_.last_event = event;
    if (event.find("CTRL-EVENT-SCAN-STARTED") != std::string::npos) {
        snapshot_.scan_started_events++;
    }
    if (event.find("CTRL-EVENT-SCAN-RESULTS") != std::string::npos) {
        snapshot_.scan_result_events++;
    }
    if (event.find("CTRL-EVENT-SCAN-FAILED") != std::string::npos) {
        snapshot_.scan_failed_events++;
    }
''',
    '''    snapshot_.last_event = event;
    ++snapshot_.event_sequence;
    if (snapshot_.event_sequence == 0) ++snapshot_.event_sequence;
    const uint64_t sequence = snapshot_.event_sequence;
    if (event.find("CTRL-EVENT-SCAN-STARTED") != std::string::npos) {
        snapshot_.scan_started_events++;
        snapshot_.last_scan_started_sequence = sequence;
    }
    if (event.find("CTRL-EVENT-SCAN-RESULTS") != std::string::npos) {
        snapshot_.scan_result_events++;
        snapshot_.last_scan_result_sequence = sequence;
    }
    if (event.find("CTRL-EVENT-SCAN-FAILED") != std::string::npos) {
        snapshot_.scan_failed_events++;
        snapshot_.last_scan_failed_sequence = sequence;
    }
''',
)

replace_once(
    "src/platform/wpa_event_monitor.cpp",
    '''       << "\\\"scan_failed_events\\\":" << snapshot.scan_failed_events << ","
       << "\\\"wifi_state\\\":\\\"" << json_escape(snapshot.wifi_state) << "\\\","
''',
    '''       << "\\\"scan_failed_events\\\":" << snapshot.scan_failed_events << ","
       << "\\\"event_sequence\\\":" << snapshot.event_sequence << ","
       << "\\\"last_scan_started_sequence\\\":" << snapshot.last_scan_started_sequence << ","
       << "\\\"last_scan_result_sequence\\\":" << snapshot.last_scan_result_sequence << ","
       << "\\\"last_scan_failed_sequence\\\":" << snapshot.last_scan_failed_sequence << ","
       << "\\\"wifi_state\\\":\\\"" << json_escape(snapshot.wifi_state) << "\\\","
''',
)

replace_once(
    "src/service/wifi_scan_lifecycle.h",
    '''struct WifiScanEventCounters {
    std::uint32_t completed = 0;
    std::uint32_t failed = 0;
};
''',
    '''struct WifiScanEventMarkers {
    std::uint64_t sequence = 0;
    std::uint64_t started_sequence = 0;
    std::uint64_t completed_sequence = 0;
    std::uint64_t failed_sequence = 0;
};
''',
)
replace_once(
    "src/service/wifi_scan_lifecycle.h",
    "    using CountersFn = std::function<WifiScanEventCounters()>;\n",
    "    using EventsFn = std::function<WifiScanEventMarkers()>;\n",
)
replace_once(
    "src/service/wifi_scan_lifecycle.h",
    "                      CountersFn counters_fn,\n",
    "                      EventsFn events_fn,\n",
)
replace_once(
    "src/service/wifi_scan_lifecycle.h",
    "    WifiScanEventCounters counters() const;\n",
    "    WifiScanEventMarkers events() const;\n",
)
replace_once(
    "src/service/wifi_scan_lifecycle.h",
    "    CountersFn counters_fn_;\n",
    "    EventsFn events_fn_;\n",
)
replace_once(
    "src/service/wifi_scan_lifecycle.h",
    "    WifiScanEventCounters baseline_;\n",
    "    WifiScanEventMarkers baseline_;\n",
)

replace_once(
    "src/service/wifi_scan_lifecycle.cpp",
    '''                                     CountersFn counters_fn,
                                     ClockFn clock_fn,
                                     std::chrono::milliseconds timeout)
    : start_fn_(std::move(start_fn)),
      results_fn_(std::move(results_fn)),
      counters_fn_(std::move(counters_fn)),
''',
    '''                                     EventsFn events_fn,
                                     ClockFn clock_fn,
                                     std::chrono::milliseconds timeout)
    : start_fn_(std::move(start_fn)),
      results_fn_(std::move(results_fn)),
      events_fn_(std::move(events_fn)),
''',
)
replace_once(
    "src/service/wifi_scan_lifecycle.cpp",
    '''WifiScanEventCounters WifiScanLifecycle::counters() const {
    return counters_fn_ ? counters_fn_() : WifiScanEventCounters{};
}
''',
    '''WifiScanEventMarkers WifiScanLifecycle::events() const {
    return events_fn_ ? events_fn_() : WifiScanEventMarkers{};
}
''',
)
replace_once(
    "src/service/wifi_scan_lifecycle.cpp",
    "    baseline_ = counters();\n",
    "    baseline_ = events();\n",
)
replace_once(
    "src/service/wifi_scan_lifecycle.cpp",
    '''    const WifiScanEventCounters current = counters();
    if (current.failed != baseline_.failed) {
        fail("scan_failed");
        return status();
    }

    if (current.completed != baseline_.completed) {
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
''',
    '''    const WifiScanEventMarkers current = events();
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
''',
)

replace_once(
    "src/service/network_daemon.cpp",
    '''            WifiScanEventCounters counters;
            counters.completed = events.scan_result_events;
            counters.failed = events.scan_failed_events;
            return counters;
''',
    '''            WifiScanEventMarkers markers;
            markers.sequence = events.event_sequence;
            markers.started_sequence = events.last_scan_started_sequence;
            markers.completed_sequence = events.last_scan_result_sequence;
            markers.failed_sequence = events.last_scan_failed_sequence;
            return markers;
''',
)

(ROOT / "tests/wifi_scan_lifecycle_test.cpp").write_text(r'''#include "service/wifi_scan_lifecycle.h"

#include <cassert>
#include <chrono>
#include <iostream>
#include <string>
#include <vector>

using namespace network_service;

int main() {
    using Clock = WifiScanLifecycle::Clock;
    Clock::time_point now{};
    WifiScanEventMarkers events;
    int start_calls = 0;
    int result_calls = 0;
    bool start_ok = true;
    std::string start_error;
    std::string result_error;
    std::vector<WifiApRecord> fake_results;
    WifiApRecord ap;
    ap.bssid = "00:11:22:33:44:55";
    ap.frequency = 2412;
    ap.signal_dbm = -42;
    ap.flags = "[WPA2-PSK-CCMP][ESS]";
    ap.ssid = "Lab";
    fake_results.push_back(ap);

    auto emit_started = [&]() {
        ++events.sequence;
        events.started_sequence = events.sequence;
    };
    auto emit_completed = [&]() {
        ++events.sequence;
        events.completed_sequence = events.sequence;
    };
    auto emit_failed = [&]() {
        ++events.sequence;
        events.failed_sequence = events.sequence;
    };

    WifiScanLifecycle lifecycle(
        [&](std::string &error) {
            ++start_calls;
            error = start_error;
            return start_ok;
        },
        [&](std::string &error) {
            ++result_calls;
            error = result_error;
            return fake_results;
        },
        [&]() { return events; },
        [&]() { return now; },
        std::chrono::milliseconds(5000));

    auto idle = lifecycle.status();
    assert(idle.scan_id == 0);
    assert(idle.state == WifiScanState::Idle);

    auto started = lifecycle.start();
    assert(started.scan_id == 1);
    assert(started.state == WifiScanState::Scanning);
    assert(start_calls == 1);
    assert(result_calls == 0);

    auto duplicate = lifecycle.start();
    assert(duplicate.scan_id == 1);
    assert(duplicate.state == WifiScanState::Scanning);
    assert(start_calls == 1);

    now += std::chrono::milliseconds(1200);
    emit_started();
    auto still_scanning = lifecycle.poll();
    assert(still_scanning.state == WifiScanState::Scanning);
    assert(result_calls == 0);

    emit_completed();
    auto ready = lifecycle.poll();
    assert(ready.state == WifiScanState::Ready);
    assert(ready.records.size() == 1);
    assert(ready.records[0].ssid == "Lab");
    assert(result_calls == 1);

    auto cached = lifecycle.poll();
    assert(cached.state == WifiScanState::Ready);
    assert(result_calls == 1);

    auto second = lifecycle.start();
    assert(second.scan_id == 2);
    assert(second.state == WifiScanState::Scanning);
    assert(start_calls == 2);
    emit_started();
    emit_failed();
    auto failed_event = lifecycle.poll();
    assert(failed_event.state == WifiScanState::Failed);
    assert(failed_event.error == "scan_failed");

    auto third = lifecycle.start();
    assert(third.scan_id == 3);
    now += std::chrono::milliseconds(5001);
    auto timed_out = lifecycle.poll();
    assert(timed_out.state == WifiScanState::Failed);
    assert(timed_out.error == "timeout");

    auto fourth = lifecycle.start();
    assert(fourth.scan_id == 4);
    emit_completed();
    auto stale_completion = lifecycle.poll();
    assert(stale_completion.state == WifiScanState::Scanning);
    assert(result_calls == 1);
    emit_started();
    auto after_new_started = lifecycle.poll();
    assert(after_new_started.state == WifiScanState::Scanning);
    emit_completed();
    auto fourth_ready = lifecycle.poll();
    assert(fourth_ready.state == WifiScanState::Ready);
    assert(result_calls == 2);

    start_ok = false;
    start_error = "backend rejected scan";
    auto fifth = lifecycle.start();
    assert(fifth.scan_id == 5);
    assert(fifth.state == WifiScanState::Failed);
    assert(fifth.error == "backend rejected scan");

    start_ok = true;
    start_error.clear();
    result_error = "read failed";
    auto sixth = lifecycle.start();
    assert(sixth.scan_id == 6);
    emit_started();
    emit_completed();
    auto read_failed = lifecycle.poll();
    assert(read_failed.state == WifiScanState::Failed);
    assert(read_failed.error == "scan_results_failed: read failed");

    std::cout << "PASS wifi_scan_lifecycle_test\n";
    return 0;
}
''', encoding="utf-8")

replace_once(
    "docs/contracts/WIFI_SCAN_LIFECYCLE_V1.md",
    '''NetworkService observes `CTRL-EVENT-SCAN-RESULTS` / `CTRL-EVENT-SCAN-FAILED` through its existing `WpaEventMonitor`. The scan lifecycle itself owns no thread and performs no fixed sleep. Result collection occurs only after the monitor observes scan completion.
''',
    '''NetworkService observes `CTRL-EVENT-SCAN-STARTED`, `CTRL-EVENT-SCAN-RESULTS`, and `CTRL-EVENT-SCAN-FAILED` through its existing `WpaEventMonitor`. The scan lifecycle itself owns no thread and performs no fixed sleep. Result collection occurs only after the monitor observes scan completion.

Every observed WPA event carries a process-local monotonic sequence marker. A terminal scan event is accepted only when its sequence is later than a `SCAN-STARTED` marker that is itself later than the baseline captured before the current start command. This generation fence prevents a late result from a timed-out scan from completing a subsequent scan generation.
''',
)

if WORKFLOW.exists():
    WORKFLOW.unlink()
if SELF.exists():
    SELF.unlink()
