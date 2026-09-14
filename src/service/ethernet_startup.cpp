#include "service/ethernet_startup.h"

#include <utility>

namespace network_service {

EthernetStartup::EthernetStartup(EthernetStartupOps ops)
    : ops_(std::move(ops)) {}

bool EthernetStartup::converge(std::string &error) {
    error.clear();
    if (!ops_.load_config) {
        error = "Ethernet config loader is unavailable";
        return false;
    }

    EthernetConfig config;
    if (!ops_.load_config(config, error)) return false;

    if (config.method == "static") {
        if (!ops_.apply_static) {
            error = "Ethernet static startup port is unavailable";
            return false;
        }
        return ops_.apply_static(config, error);
    }

    if (config.method != "dhcp") {
        error = "unsupported Ethernet mode: " + config.method;
        return false;
    }
    if (!ops_.try_adopt_dhcp || !ops_.start_dhcp) {
        error = "Ethernet DHCP startup ports are unavailable";
        return false;
    }

    switch (ops_.try_adopt_dhcp(error)) {
    case EthernetDhcpAdoption::Adopted:
        return true;
    case EthernetDhcpAdoption::Absent:
        return ops_.start_dhcp(error);
    case EthernetDhcpAdoption::Conflict:
        if (error.empty()) error = "conflicting Ethernet DHCP process";
        return false;
    case EthernetDhcpAdoption::Failed:
    default:
        if (error.empty()) error = "failed to inspect Ethernet DHCP ownership";
        return false;
    }
}

} // namespace network_service
