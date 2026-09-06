#include "service/wifi_scan_lifecycle.h"

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
