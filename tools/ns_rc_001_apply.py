#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SELF = ROOT / "tools/ns_rc_001_apply.py"
WORKFLOW = ROOT / ".github/workflows/ns-rc-001-apply.yml"


def replace_once(rel: str, old: str, new: str) -> None:
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{rel}: expected one match, found {count}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8")


def write_new(rel: str, content: str) -> None:
    path = ROOT / rel
    if path.exists():
        raise RuntimeError(f"{rel}: file already exists")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")


# 1. Split physical scan trigger from result collection while preserving legacy wifi_scan.
replace_once(
    "src/platform/wifi_backend.h",
    "std::vector<WifiApRecord> wifi_scan(const std::string &iface, std::string &error);\n",
    "bool wifi_scan_start(const std::string &iface, std::string &error);\n"
    "std::vector<WifiApRecord> wifi_scan_results(const std::string &iface, std::string &error);\n"
    "std::vector<WifiApRecord> wifi_scan(const std::string &iface, std::string &error);\n",
)

old_scan = '''std::vector<WifiApRecord> wifi_scan(const std::string &iface, std::string &error) {
    std::vector<WifiApRecord> records;
    if (!is_safe_iface(iface)) {
        error = "invalid wifi iface";
        return records;
    }
    if (!run_command("ifconfig " + iface + " up", error)) {
        return records;
    }
    (void)wpa_cli_ok(iface, "scan", error);
    usleep(1200 * 1000);

    std::string output = read_command("wpa_cli -i " + iface + " scan_results 2>/dev/null", error);
    std::istringstream input(output);
    std::string line;
    bool header = true;
    while (std::getline(input, line)) {
        line = trim(line);
        if (line.empty()) continue;
        if (header) {
            header = false;
            continue;
        }
        std::vector<std::string> fields = split_tab_line(line);
        if (fields.size() < 5) continue;
        WifiApRecord record;
        record.bssid = fields[0];
        record.frequency = atoi(fields[1].c_str());
        record.signal_dbm = atoi(fields[2].c_str());
        record.flags = fields[3];
        record.ssid = fields[4];
        records.push_back(record);
    }
    return records;
}
'''
new_scan = '''static std::vector<WifiApRecord> parse_scan_results(const std::string &output) {
    std::vector<WifiApRecord> records;
    std::istringstream input(output);
    std::string line;
    bool header = true;
    while (std::getline(input, line)) {
        line = trim(line);
        if (line.empty()) continue;
        if (header) {
            header = false;
            continue;
        }
        std::vector<std::string> fields = split_tab_line(line);
        if (fields.size() < 5) continue;
        WifiApRecord record;
        record.bssid = fields[0];
        record.frequency = atoi(fields[1].c_str());
        record.signal_dbm = atoi(fields[2].c_str());
        record.flags = fields[3];
        record.ssid = fields[4];
        records.push_back(record);
    }
    return records;
}

bool wifi_scan_start(const std::string &iface, std::string &error) {
    error.clear();
    if (!is_safe_iface(iface)) {
        error = "invalid wifi iface";
        return false;
    }
    if (!run_command("ifconfig " + iface + " up", error)) {
        return false;
    }
    return wpa_cli_ok(iface, "scan", error);
}

std::vector<WifiApRecord> wifi_scan_results(const std::string &iface, std::string &error) {
    error.clear();
    if (!is_safe_iface(iface)) {
        error = "invalid wifi iface";
        return {};
    }
    std::string output = read_command("wpa_cli -i " + iface + " scan_results 2>/dev/null", error);
    if (!error.empty() && output.empty()) return {};
    return parse_scan_results(output);
}

std::vector<WifiApRecord> wifi_scan(const std::string &iface, std::string &error) {
    std::vector<WifiApRecord> records;
    if (!is_safe_iface(iface)) {
        error = "invalid wifi iface";
        return records;
    }
    if (!run_command("ifconfig " + iface + " up", error)) {
        return records;
    }
    (void)wpa_cli_ok(iface, "scan", error);
    usleep(1200 * 1000);
    return wifi_scan_results(iface, error);
}
'''
replace_once("src/platform/wifi_backend.cpp", old_scan, new_scan)

# 2. Expose scan-completion/failure counters from the already-existing WPA monitor thread.
replace_once(
    "src/platform/wpa_event_monitor.h",
    "    uint32_t dhcp_requests = 0;\n",
    "    uint32_t dhcp_requests = 0;\n"
    "    uint32_t scan_started_events = 0;\n"
    "    uint32_t scan_result_events = 0;\n"
    "    uint32_t scan_failed_events = 0;\n",
)
replace_once(
    "src/platform/wpa_event_monitor.cpp",
    "    snapshot_.last_event = event;\n",
    "    snapshot_.last_event = event;\n"
    "    if (event.find(\"CTRL-EVENT-SCAN-STARTED\") != std::string::npos) {\n"
    "        snapshot_.scan_started_events++;\n"
    "    }\n"
    "    if (event.find(\"CTRL-EVENT-SCAN-RESULTS\") != std::string::npos) {\n"
    "        snapshot_.scan_result_events++;\n"
    "    }\n"
    "    if (event.find(\"CTRL-EVENT-SCAN-FAILED\") != std::string::npos) {\n"
    "        snapshot_.scan_failed_events++;\n"
    "    }\n",
)
replace_once(
    "src/platform/wpa_event_monitor.cpp",
    "       << \"\\\"dhcp_requests\\\":\" << snapshot.dhcp_requests << \",\"\n",
    "       << \"\\\"dhcp_requests\\\":\" << snapshot.dhcp_requests << \",\"\n"
    "       << \"\\\"scan_started_events\\\":\" << snapshot.scan_started_events << \",\"\n"
    "       << \"\\\"scan_result_events\\\":\" << snapshot.scan_result_events << \",\"\n"
    "       << \"\\\"scan_failed_events\\\":\" << snapshot.scan_failed_events << \",\"\n",
)

# 3. Add deterministic service-level lifecycle. It owns no thread.
write_new(
    "src/service/wifi_scan_lifecycle.h",
    r'''#ifndef NETWORK_SERVICE_WIFI_SCAN_LIFECYCLE_H
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

struct WifiScanEventCounters {
    std::uint32_t completed = 0;
    std::uint32_t failed = 0;
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
    using CountersFn = std::function<WifiScanEventCounters()>;
    using Clock = std::chrono::steady_clock;
    using ClockFn = std::function<Clock::time_point()>;

    WifiScanLifecycle(StartFn start_fn,
                      ResultsFn results_fn,
                      CountersFn counters_fn,
                      ClockFn clock_fn = {},
                      std::chrono::milliseconds timeout = std::chrono::milliseconds(5000));

    WifiScanStatus start();
    WifiScanStatus poll();
    WifiScanStatus status() const;

private:
    Clock::time_point now() const;
    WifiScanEventCounters counters() const;
    void fail(std::string error);

    StartFn start_fn_;
    ResultsFn results_fn_;
    CountersFn counters_fn_;
    ClockFn clock_fn_;
    std::chrono::milliseconds timeout_;

    std::uint64_t scan_id_ = 0;
    WifiScanState state_ = WifiScanState::Idle;
    std::string error_;
    std::vector<WifiApRecord> records_;
    WifiScanEventCounters baseline_;
    Clock::time_point deadline_{};
};

const char *wifi_scan_state_name(WifiScanState state);

} // namespace network_service

#endif // NETWORK_SERVICE_WIFI_SCAN_LIFECYCLE_H
''',
)
write_new(
    "src/service/wifi_scan_lifecycle.cpp",
    r'''#include "service/wifi_scan_lifecycle.h"

#include <utility>

namespace network_service {

WifiScanLifecycle::WifiScanLifecycle(StartFn start_fn,
                                     ResultsFn results_fn,
                                     CountersFn counters_fn,
                                     ClockFn clock_fn,
                                     std::chrono::milliseconds timeout)
    : start_fn_(std::move(start_fn)),
      results_fn_(std::move(results_fn)),
      counters_fn_(std::move(counters_fn)),
      clock_fn_(std::move(clock_fn)),
      timeout_(timeout.count() > 0 ? timeout : std::chrono::milliseconds(5000)) {}

WifiScanLifecycle::Clock::time_point WifiScanLifecycle::now() const {
    return clock_fn_ ? clock_fn_() : Clock::now();
}

WifiScanEventCounters WifiScanLifecycle::counters() const {
    return counters_fn_ ? counters_fn_() : WifiScanEventCounters{};
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
    baseline_ = counters();
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

    const WifiScanEventCounters current = counters();
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
''',
)

# 4. Wire lifecycle into NetworkDaemon with additive non-blocking business methods.
replace_once(
    "src/service/network_daemon.h",
    "class WpaEventMonitor;\n",
    "class WpaEventMonitor;\nclass WifiScanLifecycle;\n",
)
replace_once(
    "src/service/network_daemon.h",
    "    std::string wifi_scan_json() const;\n",
    "    std::string wifi_scan_json() const;\n"
    "    std::string wifi_scan_start_json();\n"
    "    std::string wifi_scan_status_json();\n",
)
replace_once(
    "src/service/network_daemon.h",
    "    std::unique_ptr<WpaEventMonitor> wpa_monitor_;\n",
    "    std::unique_ptr<WpaEventMonitor> wpa_monitor_;\n"
    "    std::unique_ptr<WifiScanLifecycle> wifi_scan_lifecycle_;\n",
)
replace_once(
    "src/service/network_daemon.cpp",
    "#include \"platform/wpa_event_monitor.h\"\n",
    "#include \"platform/wpa_event_monitor.h\"\n#include \"service/wifi_scan_lifecycle.h\"\n",
)
replace_once(
    "src/service/network_daemon.cpp",
    '''      wpa_monitor_(new WpaEventMonitor(wifi_iface_, std::move(event_dir))) {
    wpa_monitor_->start();
}
''',
    '''      wpa_monitor_(new WpaEventMonitor(wifi_iface_, std::move(event_dir))) {
    wpa_monitor_->start();
    wifi_scan_lifecycle_.reset(new WifiScanLifecycle(
        [this](std::string &error) {
            return wifi_scan_start(wifi_iface_, error);
        },
        [this](std::string &error) {
            return wifi_scan_results(wifi_iface_, error);
        },
        [this]() {
            const WpaEventSnapshot events = wpa_monitor_->snapshot();
            WifiScanEventCounters counters;
            counters.completed = events.scan_result_events;
            counters.failed = events.scan_failed_events;
            return counters;
        }));
}
''',
)
replace_once(
    "src/service/network_daemon.cpp",
    '''std::string NetworkDaemon::wifi_scan_json() const {
    std::string error;
    std::vector<WifiApRecord> records = wifi_scan(wifi_iface_, error);
    if (!error.empty() && records.empty()) {
        return error_json(500, error);
    }
    return ok_json(wifi_scan_to_json(records));
}
''',
    '''std::string NetworkDaemon::wifi_scan_json() const {
    std::string error;
    std::vector<WifiApRecord> records = wifi_scan(wifi_iface_, error);
    if (!error.empty() && records.empty()) {
        return error_json(500, error);
    }
    return ok_json(wifi_scan_to_json(records));
}

static std::string wifi_scan_lifecycle_json(const WifiScanStatus &status) {
    std::ostringstream os;
    os << "{\"scanId\":" << status.scan_id
       << ",\"state\":\"" << wifi_scan_state_name(status.state) << "\""
       << ",\"error\":\"" << json_escape(status.error) << "\""
       << ",\"results\":" << wifi_scan_to_json(status.records)
       << "}";
    return os.str();
}

std::string NetworkDaemon::wifi_scan_start_json() {
    if (!wifi_scan_lifecycle_) {
        return error_json(500, "wifi scan lifecycle unavailable");
    }
    WifiScanStatus status = wifi_scan_lifecycle_->start();
    if (status.state == WifiScanState::Failed) {
        return error_json(500, status.error);
    }
    return std::string("{\"status\":202,\"result\":") +
           wifi_scan_lifecycle_json(status) + "}";
}

std::string NetworkDaemon::wifi_scan_status_json() {
    if (!wifi_scan_lifecycle_) {
        return error_json(500, "wifi scan lifecycle unavailable");
    }
    return ok_json(wifi_scan_lifecycle_json(wifi_scan_lifecycle_->poll()));
}
''',
)

replace_once(
    "src/ipc/network_ipc_v1_business_dispatch.cpp",
    '''    } else if (method == "wifi.scan") {
        daemon_json = daemon.wifi_scan_json();
''',
    '''    } else if (method == "wifi.scan.start") {
        daemon_json = daemon.wifi_scan_start_json();
    } else if (method == "wifi.scan.status") {
        daemon_json = daemon.wifi_scan_status_json();
    } else if (method == "wifi.scan") {
        daemon_json = daemon.wifi_scan_json();
''',
)

# 5. Deterministic lifecycle regression.
write_new(
    "tests/wifi_scan_lifecycle_test.cpp",
    r'''#include "service/wifi_scan_lifecycle.h"

#include <cassert>
#include <chrono>
#include <iostream>
#include <string>
#include <vector>

using namespace network_service;

int main() {
    using Clock = WifiScanLifecycle::Clock;
    Clock::time_point now{};
    WifiScanEventCounters counters;
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
        [&]() { return counters; },
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
    auto still_scanning = lifecycle.poll();
    assert(still_scanning.state == WifiScanState::Scanning);
    assert(result_calls == 0);

    ++counters.completed;
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
    ++counters.failed;
    auto failed_event = lifecycle.poll();
    assert(failed_event.state == WifiScanState::Failed);
    assert(failed_event.error == "scan_failed");

    auto third = lifecycle.start();
    assert(third.scan_id == 3);
    now += std::chrono::milliseconds(5001);
    auto timed_out = lifecycle.poll();
    assert(timed_out.state == WifiScanState::Failed);
    assert(timed_out.error == "timeout");

    start_ok = false;
    start_error = "backend rejected scan";
    auto fourth = lifecycle.start();
    assert(fourth.scan_id == 4);
    assert(fourth.state == WifiScanState::Failed);
    assert(fourth.error == "backend rejected scan");

    start_ok = true;
    start_error.clear();
    result_error = "read failed";
    auto fifth = lifecycle.start();
    assert(fifth.scan_id == 5);
    ++counters.completed;
    auto read_failed = lifecycle.poll();
    assert(read_failed.state == WifiScanState::Failed);
    assert(read_failed.error == "scan_results_failed: read failed");

    std::cout << "PASS wifi_scan_lifecycle_test\n";
    return 0;
}
''',
)

replace_once(
    "CMakeLists.txt",
    "    src/service/network_state_change_detector.cpp\n",
    "    src/service/network_state_change_detector.cpp\n    src/service/wifi_scan_lifecycle.cpp\n",
)
replace_once(
    "CMakeLists.txt",
    '''    add_executable(network_state_change_detector_test
        tests/network_state_change_detector_test.cpp
        src/service/network_state_change_detector.cpp
    )
''',
    '''    add_executable(wifi_scan_lifecycle_test
        tests/wifi_scan_lifecycle_test.cpp
        src/service/wifi_scan_lifecycle.cpp
    )
    target_include_directories(wifi_scan_lifecycle_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    add_test(NAME wifi_scan_lifecycle COMMAND wifi_scan_lifecycle_test)

    add_executable(network_state_change_detector_test
        tests/network_state_change_detector_test.cpp
        src/service/network_state_change_detector.cpp
    )
''',
)
replace_once(
    "CMakeLists.txt",
    "        src/service/network_state_change_detector.cpp\n        src/ipc/network_ipc_server.cpp\n",
    "        src/service/network_state_change_detector.cpp\n        src/service/wifi_scan_lifecycle.cpp\n        src/ipc/network_ipc_server.cpp\n",
)

# 6. Host dispatch regression: status must be immediate and side-effect-free when idle.
replace_once(
    "tests/ipc_v1_business_dispatch_test.py",
    '''def test_unknown_method_correlated_404(path: Path):
''',
    '''def test_scan_status_is_immediate_and_explicit(path: Path):
    with connect_ready(path) as sock:
        started = time.monotonic()
        response = request(sock, 45, "wifi.scan.status", {})
        elapsed = time.monotonic() - started
        if elapsed > 0.5:
            raise AssertionError(f"wifi.scan.status blocked for {elapsed:.3f}s")
        if response.get("status") != 200:
            raise AssertionError(f"wifi.scan.status failed: {response}")
        result = response.get("result") or {}
        if result.get("state") != "idle" or result.get("scanId") != 0:
            raise AssertionError(f"unexpected idle scan state: {response}")
        results = result.get("results") or {}
        if results.get("count") != 0 or results.get("aps") != []:
            raise AssertionError(f"idle scan results must be empty: {response}")


def test_unknown_method_correlated_404(path: Path):
''',
)
replace_once(
    "tests/ipc_v1_business_dispatch_test.py",
    '''        test_boolean_schema_is_strict,
        test_unknown_method_correlated_404,
''',
    '''        test_boolean_schema_is_strict,
        test_scan_status_is_immediate_and_explicit,
        test_unknown_method_correlated_404,
''',
)

# 7. Freeze the additive lifecycle contract and thread ownership.
write_new(
    "docs/contracts/WIFI_SCAN_LIFECYCLE_V1.md",
    r'''# Wi-Fi Scan Lifecycle v1 Addendum

Status: **FROZEN for NS-RC-001**

This addendum extends the frozen Network IPC v1 business method surface without changing the v1 frame format, request correlation, event sequencing, reconnect, or snapshot-rebase rules.

## Goal

A physical Wi-Fi scan MUST NOT keep a SmartControl/LVGL synchronous IPC call blocked for the physical scan duration.

## Methods

### `wifi.scan.start`

REQUEST params are `{}`.

A successful start returns promptly with HTTP-like status `202`:

```json
{
  "requestId": 1,
  "status": 202,
  "result": {
    "scanId": 7,
    "state": "scanning",
    "error": "",
    "results": {"count": 0, "aps": []}
  }
}
```

Rules:

1. At most one physical scan is owned by NetworkService at a time.
2. A repeated `wifi.scan.start` while state is `scanning` is idempotent: it returns the same `scanId` and MUST NOT issue a second physical scan.
3. Starting after `ready` or `failed` allocates a new monotonically increasing process-local `scanId`.
4. Immediate backend rejection returns a correlated non-2xx RESPONSE; no SmartControl fallback to `wpa_cli` is permitted.

### `wifi.scan.status`

REQUEST params are `{}`. It never waits for the physical scan to finish.

Successful RESPONSE uses status `200` and one of four states:

- `idle`: no scan has been started in this process.
- `scanning`: a physical scan is in flight.
- `ready`: `results` contains the authoritative scan result owned by NetworkService.
- `failed`: `error` is non-empty; `timeout` is the stable timeout reason.

`results` always has the shape `{ "count": N, "aps": [...] }`; before `ready`, it is empty.

## Completion ownership

NetworkService observes `CTRL-EVENT-SCAN-RESULTS` / `CTRL-EVENT-SCAN-FAILED` through its existing `WpaEventMonitor`. The scan lifecycle itself owns no thread and performs no fixed sleep. Result collection occurs only after the monitor observes scan completion.

The current implementation timeout is 5000 ms. Timeout handling is bounded and explicit; increasing SmartControl's synchronous I/O timeout is not an allowed substitute.

## Compatibility boundary

Legacy `wifi.scan` remains temporarily available for brownfield compatibility and retains its synchronous behavior. New/updated SmartControl code MUST migrate to `wifi.scan.start` + `wifi.scan.status`. Removal of legacy `wifi.scan` requires consumer migration and joint RC evidence.
''',
)
replace_once(
    "docs/engineering/thread-model.md",
    "## WPA event monitor\n",
    '''### Reactor-owned Wi-Fi scan lifecycle

NS-RC-001 adds a process-local `WifiScanLifecycle` state machine owned by `NetworkDaemon`. Starting a scan issues only the bounded backend trigger and returns immediately. `wifi.scan.status` polls the state machine without waiting for physical completion. The existing `WpaEventMonitor` worker only contributes scan-completed/scan-failed counters; result collection occurs after those counters advance. No scan worker thread and no fixed sleep is added to the IPC reactor path.

## WPA event monitor
''',
)

# One-shot tooling must not remain in the product branch.
if WORKFLOW.exists():
    WORKFLOW.unlink()
if SELF.exists():
    SELF.unlink()
