#include "service/network_state.h"

#include <iostream>
#include <string>

namespace {

bool expect(bool condition, const char *message) {
    if (condition) return true;
    std::cerr << "network_state_test: " << message << std::endl;
    return false;
}

network_service::NetworkSnapshot base_snapshot() {
    network_service::NetworkSnapshot out;
    out.eth.iface = "eth0";
    out.wifi.iface = "wlan0";
    return out;
}

} // namespace

int main() {
    using network_service::IpState;
    using network_service::WifiL2State;
    using network_service::WifiRuntimeFact;
    using network_service::legacy_wifi_state;
    using network_service::normalize_network_snapshot;

    bool ok = true;

    {
        auto snapshot = base_snapshot();
        WifiRuntimeFact runtime;
        runtime.l2_state = WifiL2State::Connected;
        runtime.dhcp_requested = true;
        normalize_network_snapshot(snapshot, runtime);

        ok = expect(snapshot.wifi.connected,
                    "L2 CONNECTED must not wait for DHCP to become connected") && ok;
        ok = expect(snapshot.wifi.ip_state == IpState::Configuring,
                    "L2 CONNECTED without IP must be IP Configuring") && ok;
        ok = expect(!snapshot.network_ready && !snapshot.online,
                    "L2-only connection must not be network ready") && ok;
        ok = expect(legacy_wifi_state(snapshot, runtime) == "ip_configuring",
                    "legacy WPA state must remain ip_configuring") && ok;
    }

    {
        auto snapshot = base_snapshot();
        snapshot.wifi.has_ip = true;
        snapshot.wifi.ip4 = "10.0.0.20";
        snapshot.wifi.has_default_route = true;
        snapshot.wifi.gateway4 = "10.0.0.1";
        snapshot.wifi.route_metric = 20;
        snapshot.dns_available = true;
        snapshot.dns4 = "8.8.8.8";

        WifiRuntimeFact runtime;
        runtime.l2_state = WifiL2State::Connected;
        runtime.dhcp_requested = true;
        normalize_network_snapshot(snapshot, runtime);

        ok = expect(snapshot.wifi.ip_state == IpState::Ready,
                    "connected Wi-Fi with IPv4 must be IP Ready") && ok;
        ok = expect(snapshot.primary_iface == "wlan0",
                    "usable Wi-Fi route must become primary") && ok;
        ok = expect(snapshot.network_ready && snapshot.online,
                    "route + DNS on connected Wi-Fi must be network ready") && ok;
        ok = expect(legacy_wifi_state(snapshot, runtime) == "connected",
                    "legacy WPA state must become connected only after L3 readiness") && ok;
    }

    {
        auto snapshot = base_snapshot();
        // Kernel state can lag an L2 disconnect. Keep these raw facts visible,
        // but they must not make the interface usable.
        snapshot.wifi.has_ip = true;
        snapshot.wifi.ip4 = "10.0.0.20";
        snapshot.wifi.has_default_route = true;
        snapshot.wifi.gateway4 = "10.0.0.1";
        snapshot.wifi.route_metric = 20;
        snapshot.dns_available = true;
        snapshot.dns4 = "8.8.8.8";

        WifiRuntimeFact runtime;
        runtime.l2_state = WifiL2State::Disconnected;
        normalize_network_snapshot(snapshot, runtime);

        ok = expect(snapshot.wifi.has_ip,
                    "raw stale IP fact must remain observable") && ok;
        ok = expect(!snapshot.wifi.connected,
                    "explicit L2 disconnect must override stale IPv4") && ok;
        ok = expect(snapshot.wifi.ip_state == IpState::None,
                    "disconnected Wi-Fi must not report IP Ready") && ok;
        ok = expect(snapshot.primary_iface.empty() && !snapshot.network_ready,
                    "stale Wi-Fi route must not create readiness") && ok;
        ok = expect(legacy_wifi_state(snapshot, runtime) == "disconnected",
                    "legacy state must follow explicit L2 disconnect") && ok;
    }

    {
        auto snapshot = base_snapshot();
        // Brownfield startup compatibility: NetworkService may attach after an
        // already-live supplicant connection and not yet have an L2 event.
        snapshot.wifi.has_ip = true;
        snapshot.wifi.has_default_route = true;
        snapshot.wifi.route_metric = 20;
        snapshot.dns_available = true;
        snapshot.dns4 = "1.1.1.1";

        WifiRuntimeFact runtime;
        runtime.l2_state = WifiL2State::Unknown;
        normalize_network_snapshot(snapshot, runtime);

        ok = expect(snapshot.wifi.connected &&
                    snapshot.wifi.ip_state == IpState::Ready,
                    "Unknown L2 may adopt an already-live Wi-Fi link") && ok;
        ok = expect(snapshot.network_ready,
                    "brownfield live Wi-Fi link must remain usable") && ok;
    }

    {
        auto snapshot = base_snapshot();
        snapshot.eth.carrier_up = true;
        snapshot.eth.has_ip = true;
        snapshot.eth.has_default_route = true;
        snapshot.eth.route_metric = 10;
        snapshot.wifi.has_ip = true;
        snapshot.wifi.has_default_route = true;
        snapshot.wifi.route_metric = 20;
        snapshot.dns_available = true;
        snapshot.dns4 = "1.1.1.1";

        WifiRuntimeFact runtime;
        runtime.l2_state = WifiL2State::Connected;
        normalize_network_snapshot(snapshot, runtime);

        ok = expect(snapshot.eth.ip_state == IpState::Ready && snapshot.eth.connected,
                    "Ethernet link/IP truth must normalize independently") && ok;
        ok = expect(snapshot.primary_iface == "eth0",
                    "lower-metric usable Ethernet must remain primary") && ok;
    }

    return ok ? 0 : 1;
}
