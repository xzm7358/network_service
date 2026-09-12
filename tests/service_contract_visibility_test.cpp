#if __has_include("platform/wifi_backend.h")
#error "Service consumers must not receive Platform include visibility"
#endif

#if __has_include("platform/dhcp_lease_store.h")
#error "Service consumers must not receive DHCP Platform include visibility"
#endif

#if __has_include("config/ethernet_config.h")
#error "Service consumers must not receive Config include visibility"
#endif

#include "network_service_contracts.h"
#include "service/network_control_plane.h"
#include "service/network_daemon.h"
#include "service/wifi_scan_lifecycle.h"

#include <type_traits>

int main() {
    static_assert(std::is_default_constructible<network_service::EthernetConfig>::value,
                  "EthernetConfig must be a neutral contract");
    static_assert(std::is_default_constructible<network_service::WifiApRecord>::value,
                  "WifiApRecord must be a neutral contract");
    static_assert(std::is_default_constructible<network_service::WifiSavedNetwork>::value,
                  "WifiSavedNetwork must be a neutral contract");
    static_assert(std::is_default_constructible<network_service::DhcpLeaseFact>::value,
                  "DhcpLeaseFact must be a neutral contract");
    return 0;
}
