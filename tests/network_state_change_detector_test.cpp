#include "service/network_state_change_detector.h"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

network_service::NetworkSnapshot baseline_snapshot() {
    network_service::NetworkSnapshot snapshot;
    snapshot.eth.iface = "eth0";
    snapshot.eth.exists = true;
    snapshot.eth.enabled = true;
    snapshot.eth.carrier_up = false;
    snapshot.eth.connected = false;
    snapshot.eth.route_metric = 10;

    snapshot.wifi.iface = "wlan0";
    snapshot.wifi.exists = true;
    snapshot.wifi.enabled = true;
    snapshot.wifi.carrier_up = true;
    snapshot.wifi.connected = true;
    snapshot.wifi.has_ip = true;
    snapshot.wifi.ip4 = "192.168.1.20";
    snapshot.wifi.netmask4 = "255.255.255.0";
    snapshot.wifi.has_default_route = true;
    snapshot.wifi.gateway4 = "192.168.1.1";
    snapshot.wifi.route_metric = 20;
    snapshot.wifi.ssid = "Home";
    snapshot.wifi.signal_dbm = -48;
    snapshot.wifi.signal_bars = 4;

    snapshot.primary_iface = "wlan0";
    snapshot.online = true;
    snapshot.dns_available = true;
    snapshot.dns4 = "1.1.1.1";
    return snapshot;
}

void test_first_observation_establishes_baseline() {
    network_service::NetworkStateChangeDetector detector;
    const auto changes = detector.observe(baseline_snapshot());
    require(detector.initialized(), "detector did not initialize");
    require(!changes.any(), "first observation must not emit a change");
    require(changes.payload_json() == "{\"changed\":[]}",
            "empty payload contract changed");
}

void test_identical_snapshot_is_quiet() {
    network_service::NetworkStateChangeDetector detector;
    const auto baseline = baseline_snapshot();
    (void)detector.observe(baseline);
    require(!detector.observe(baseline).any(),
            "identical snapshot must not emit a change");
}

void test_raw_signal_dbm_is_not_a_trigger() {
    network_service::NetworkStateChangeDetector detector;
    auto snapshot = baseline_snapshot();
    (void)detector.observe(snapshot);
    snapshot.wifi.signal_dbm = -57;
    const auto changes = detector.observe(snapshot);
    require(!changes.any(), "raw signal_dbm alone must not emit an EVENT");
}

void test_signal_bars_are_ui_visible_wifi_state() {
    network_service::NetworkStateChangeDetector detector;
    auto snapshot = baseline_snapshot();
    (void)detector.observe(snapshot);
    snapshot.wifi.signal_dbm = -57;
    snapshot.wifi.signal_bars = 3;
    const auto changes = detector.observe(snapshot);
    require(changes.wifi && !changes.eth && !changes.route && !changes.dns,
            "signal_bars change must classify as wifi");
    require(changes.payload_json() == "{\"changed\":[\"wifi\"]}",
            "wifi payload contract changed");
}

void test_eth_state_change() {
    network_service::NetworkStateChangeDetector detector;
    auto snapshot = baseline_snapshot();
    (void)detector.observe(snapshot);
    snapshot.eth.carrier_up = true;
    snapshot.eth.connected = true;
    snapshot.eth.has_ip = true;
    snapshot.eth.ip4 = "10.0.0.20";
    const auto changes = detector.observe(snapshot);
    require(changes.eth && !changes.wifi && !changes.route && !changes.dns,
            "ethernet transition classification failed");
}

void test_route_and_dns_change_have_stable_order() {
    network_service::NetworkStateChangeDetector detector;
    auto snapshot = baseline_snapshot();
    (void)detector.observe(snapshot);
    snapshot.primary_iface = "eth0";
    snapshot.online = false;
    snapshot.dns_available = false;
    snapshot.dns4.clear();
    const auto changes = detector.observe(snapshot);
    require(!changes.eth && !changes.wifi && changes.route && changes.dns,
            "route/dns transition classification failed");
    require(changes.payload_json() ==
                "{\"changed\":[\"route\",\"dns\"]}",
            "changed category order is not stable");
}

void test_multiple_categories_share_one_change_set() {
    network_service::NetworkStateChangeDetector detector;
    auto snapshot = baseline_snapshot();
    (void)detector.observe(snapshot);
    snapshot.eth.carrier_up = true;
    snapshot.wifi.connected = false;
    snapshot.wifi.has_ip = false;
    snapshot.wifi.ip4.clear();
    snapshot.primary_iface.clear();
    snapshot.online = false;
    snapshot.dns_available = false;
    snapshot.dns4.clear();
    const auto changes = detector.observe(snapshot);
    require(changes.eth && changes.wifi && changes.route && changes.dns,
            "multi-category transition classification failed");
    require(changes.payload_json() ==
                "{\"changed\":[\"eth\",\"wifi\",\"route\",\"dns\"]}",
            "multi-category payload contract changed");
}

void test_reset_requires_new_baseline() {
    network_service::NetworkStateChangeDetector detector;
    auto snapshot = baseline_snapshot();
    (void)detector.observe(snapshot);
    detector.reset();
    snapshot.wifi.ssid = "Other";
    require(!detector.observe(snapshot).any(),
            "first observation after reset must be a baseline");
}

} // namespace

int main() {
    try {
        test_first_observation_establishes_baseline();
        test_identical_snapshot_is_quiet();
        test_raw_signal_dbm_is_not_a_trigger();
        test_signal_bars_are_ui_visible_wifi_state();
        test_eth_state_change();
        test_route_and_dns_change_have_stable_order();
        test_multiple_categories_share_one_change_set();
        test_reset_requires_new_baseline();
        std::cout << "Network state change detector contract tests passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "Network state change detector test failed: " << e.what() << '\n';
        return 1;
    }
}
