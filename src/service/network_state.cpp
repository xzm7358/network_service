#include "service/network_state.h"

namespace network_service {

bool wifi_l2_connected(WifiL2State state) {
    return state == WifiL2State::Connected;
}

const char *wifi_l2_state_name(WifiL2State state) {
    switch (state) {
    case WifiL2State::Disabled: return "disabled";
    case WifiL2State::Disconnected: return "disconnected";
    case WifiL2State::Scanning: return "scanning";
    case WifiL2State::Associating: return "associating";
    case WifiL2State::Associated: return "associated";
    case WifiL2State::Handshake: return "handshake";
    case WifiL2State::Connected: return "connected";
    case WifiL2State::Failed: return "failed";
    case WifiL2State::Unknown:
    default: return "unknown";
    }
}

const char *ip_state_name(IpState state) {
    switch (state) {
    case IpState::Configuring: return "configuring";
    case IpState::Ready: return "ready";
    case IpState::None:
    default: return "none";
    }
}

void normalize_network_snapshot(NetworkSnapshot &snapshot,
                                const WifiRuntimeFact &wifi_runtime) {
    snapshot.eth.ip_state = snapshot.eth.has_ip ? IpState::Ready : IpState::None;
    snapshot.eth.connected = snapshot.eth.carrier_up &&
                             snapshot.eth.ip_state == IpState::Ready;

    snapshot.wifi.wifi_l2_state = wifi_runtime.l2_state;

    // A daemon may adopt an already-live Wi-Fi link before observing a WPA
    // CONNECTED event. Preserve brownfield compatibility only for Unknown; once
    // WPA has emitted an explicit L2 state, that state is authoritative.
    const bool compat_live_link = wifi_runtime.l2_state == WifiL2State::Unknown &&
                                  snapshot.wifi.has_ip;
    const bool l2_connected = wifi_l2_connected(wifi_runtime.l2_state) ||
                              compat_live_link;
    snapshot.wifi.connected = l2_connected;

    if (l2_connected && snapshot.wifi.has_ip) {
        snapshot.wifi.ip_state = IpState::Ready;
    } else if (l2_connected && wifi_runtime.dhcp_requested) {
        snapshot.wifi.ip_state = IpState::Configuring;
    } else {
        snapshot.wifi.ip_state = IpState::None;
    }

    const bool eth_usable = snapshot.eth.connected &&
                            snapshot.eth.has_default_route;
    const bool wifi_usable = snapshot.wifi.connected &&
                             snapshot.wifi.ip_state == IpState::Ready &&
                             snapshot.wifi.has_default_route;

    snapshot.primary_iface.clear();
    if (eth_usable) snapshot.primary_iface = snapshot.eth.iface;
    if (wifi_usable &&
        (!eth_usable || snapshot.wifi.route_metric < snapshot.eth.route_metric)) {
        snapshot.primary_iface = snapshot.wifi.iface;
    }

    snapshot.network_ready = !snapshot.primary_iface.empty() &&
                             snapshot.dns_available;
    snapshot.online = snapshot.network_ready;
}

std::string legacy_wifi_state(const NetworkSnapshot &snapshot,
                              const WifiRuntimeFact &wifi_runtime) {
    if (!wifi_runtime.failure_reason.empty() ||
        wifi_runtime.l2_state == WifiL2State::Failed) {
        return "failed";
    }

    switch (wifi_runtime.l2_state) {
    case WifiL2State::Disabled:
        return "disabled";
    case WifiL2State::Disconnected:
        return "disconnected";
    case WifiL2State::Scanning:
        return "scanning";
    case WifiL2State::Associating:
        return "associating";
    case WifiL2State::Associated:
        return "associated";
    case WifiL2State::Handshake:
        return "handshake";
    case WifiL2State::Connected:
        if (snapshot.wifi.ip_state == IpState::Ready &&
            snapshot.wifi.has_default_route && snapshot.dns_available) {
            return "connected";
        }
        return wifi_runtime.dhcp_requested ? "ip_configuring" : "associated";
    case WifiL2State::Unknown:
    default:
        // Preserve startup compatibility when NetworkService adopts an already
        // configured interface before observing a supplicant transition.
        if (snapshot.wifi.connected && snapshot.wifi.ip_state == IpState::Ready) {
            if (snapshot.wifi.has_default_route && snapshot.dns_available) {
                return "connected";
            }
            return "ip_configuring";
        }
        return "disconnected";
    }
}

} // namespace network_service
