#ifndef NETWORK_SERVICE_NETWORK_STATE_H
#define NETWORK_SERVICE_NETWORK_STATE_H

#include <string>

#include "network_service_types.h"

namespace network_service {

struct WifiRuntimeFact {
    WifiL2State l2_state = WifiL2State::Unknown;
    bool dhcp_requested = false;
    std::string failure_reason;
};

bool wifi_l2_connected(WifiL2State state);
const char *wifi_l2_state_name(WifiL2State state);
const char *ip_state_name(IpState state);

// Converts independent platform facts (link/IP/route/DNS) into one normalized
// Service truth model. This function is pure and performs no network mutation.
void normalize_network_snapshot(NetworkSnapshot &snapshot,
                                const WifiRuntimeFact &wifi_runtime);

// Legacy `wpa.events.wifi_state` projection retained for IPC compatibility.
// New Service/Policy code must use WifiL2State + IpState instead.
std::string legacy_wifi_state(const NetworkSnapshot &snapshot,
                              const WifiRuntimeFact &wifi_runtime);

} // namespace network_service

#endif // NETWORK_SERVICE_NETWORK_STATE_H
