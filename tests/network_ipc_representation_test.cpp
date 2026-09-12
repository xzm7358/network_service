#include "ipc/network_ipc_representation.h"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

void require_equal(const std::string &actual,
                   const std::string &expected,
                   const char *message) {
    if (actual != expected) {
        std::cerr << message << "\nexpected: " << expected
                  << "\nactual:   " << actual << '\n';
        throw std::runtime_error(message);
    }
}

void test_state_change_payload_order() {
    network_service::NetworkStateChangeSet changes;
    require_equal(network_service::ipc_representation::state_changes_payload(changes),
                  "{\"changed\":[]}",
                  "empty state-change payload changed");

    changes.wifi = true;
    require_equal(network_service::ipc_representation::state_changes_payload(changes),
                  "{\"changed\":[\"wifi\"]}",
                  "single state-change payload changed");

    changes.eth = true;
    changes.route = true;
    changes.dns = true;
    require_equal(network_service::ipc_representation::state_changes_payload(changes),
                  "{\"changed\":[\"eth\",\"wifi\",\"route\",\"dns\"]}",
                  "state-change category order changed");
}

void test_v0_envelopes_and_escape() {
    require_equal(network_service::ipc_representation::v0_success(
                      200, "{\"requested\":\"disconnect\"}"),
                  "{\"status\":200,\"result\":{\"requested\":\"disconnect\"}}",
                  "v0 success envelope changed");
    require_equal(network_service::ipc_representation::v0_error(500, "bad \"value\"\n"),
                  "{\"status\":500,\"error\":\"bad \\\"value\\\"\\n\"}",
                  "v0 error escaping changed");
}

void test_wifi_command_payloads() {
    network_service::WifiCommandResult command;
    command.requested = "connect";
    require_equal(network_service::ipc_representation::wifi_command_payload(command),
                  "{\"requested\":\"connect\"}",
                  "Wi-Fi command payload changed");

    command.requested = "autoconnect";
    command.has_enabled = true;
    command.enabled = false;
    require_equal(network_service::ipc_representation::wifi_command_payload(command),
                  "{\"requested\":\"autoconnect\",\"enabled\":false}",
                  "Wi-Fi autoconnect payload changed");
}

void test_ethernet_config_payload() {
    network_service::EthernetConfig config;
    config.iface = "eth0";
    config.method = "static";
    config.ip4 = "10.0.0.2";
    config.netmask4 = "255.255.255.0";
    config.gateway4 = "10.0.0.1";
    config.dns4 = "1.1.1.1";
    config.route_metric = 10;
    config.dns_enabled = true;
    require_equal(
        network_service::ipc_representation::ethernet_config_payload(config),
        "{\"iface\":\"eth0\",\"method\":\"static\",\"ip4\":\"10.0.0.2\","
        "\"netmask4\":\"255.255.255.0\",\"gateway4\":\"10.0.0.1\","
        "\"dns4\":\"1.1.1.1\",\"route_metric\":10,\"dns_enabled\":true}",
        "Ethernet config payload changed");
}

void test_snapshot_payload_projects_policy() {
    network_service::NetworkSnapshot snapshot;
    snapshot.eth.iface = "eth0";
    snapshot.wifi.iface = "wlan0";
    snapshot.primary_iface = "wlan0";
    snapshot.online = true;
    snapshot.dns_available = true;
    snapshot.dns4 = "8.8.8.8";
    snapshot.route_policy = network_service::RoutePolicy::WifiPreferred;
    snapshot.dns_policy = network_service::DnsPolicy::Overwrite;

    const std::string payload =
        network_service::ipc_representation::snapshot_payload(snapshot);
    require(payload.find("\"route_policy\":\"wifi_preferred\"") != std::string::npos,
            "snapshot route policy projection changed");
    require(payload.find("\"dns_policy\":\"overwrite\"") != std::string::npos,
            "snapshot DNS policy projection changed");
    require(payload.find("\"primary_iface\":\"wlan0\"") != std::string::npos,
            "snapshot primary interface projection changed");
}

void test_wifi_scan_and_saved_payloads() {
    network_service::WifiApRecord ap;
    ap.bssid = "aa:bb:cc:dd:ee:ff";
    ap.frequency = 2412;
    ap.signal_dbm = -42;
    ap.flags = "[WPA2-PSK-CCMP][ESS]";
    ap.ssid = "Home\"Lab";
    const std::vector<network_service::WifiApRecord> aps{ap};
    const std::string scan = network_service::ipc_representation::wifi_scan_payload(aps);
    require(scan.find("\"count\":1") != std::string::npos,
            "Wi-Fi scan count changed");
    require(scan.find("Home\\\"Lab") != std::string::npos,
            "Wi-Fi scan escaping changed");

    network_service::WifiSavedNetwork saved;
    saved.network_id = 3;
    saved.ssid = "Home";
    saved.is_current = true;
    saved.autoconnect = true;
    const std::vector<network_service::WifiSavedNetwork> records{saved};
    const std::string payload =
        network_service::ipc_representation::wifi_saved_payload(records);
    require(payload.find("\"network_id\":3") != std::string::npos,
            "saved network id changed");
    require(payload.find("\"current\":true") != std::string::npos,
            "saved network current flag changed");
}

} // namespace

int main() {
    try {
        test_state_change_payload_order();
        test_v0_envelopes_and_escape();
        test_wifi_command_payloads();
        test_ethernet_config_payload();
        test_snapshot_payload_projects_policy();
        test_wifi_scan_and_saved_payloads();
        std::cout << "IPC representation contract tests passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "IPC representation test failed: " << e.what() << '\n';
        return 1;
    }
}
