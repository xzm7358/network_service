#include "platform/ethernet_apply.h"

#include <string>

#include "platform/network_configurator.h"
#include "platform/udhcpc_process.h"

namespace network_service {

namespace {

static bool is_safe_iface(const std::string &value) {
    if (value.empty() || value.size() > 15) return false;
    for (char ch : value) {
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.')) {
            return false;
        }
    }
    return true;
}

static bool validate_config_common(const EthernetConfig &config, std::string &error) {
    if (!is_safe_iface(config.iface)) {
        error = "invalid iface";
        return false;
    }
    return true;
}

} // namespace

bool apply_ethernet_static(const EthernetConfig &config, std::string &error) {
    if (!validate_config_common(config, error)) return false;
    if (config.ip4.empty() || config.netmask4.empty()) {
        error = "static IP and netmask are required";
        return false;
    }

    UdhcpcProcess::stop(config.iface);
    return NetworkConfigurator::apply_ipv4(config.iface,
                                           config.ip4,
                                           config.netmask4,
                                           error);
}

bool apply_ethernet_dhcp(const EthernetConfig &config, std::string &error) {
    if (!validate_config_common(config, error)) return false;
    return UdhcpcProcess::start(config.iface, error);
}

} // namespace network_service
