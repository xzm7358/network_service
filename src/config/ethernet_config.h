#ifndef NETWORK_SERVICE_ETHERNET_CONFIG_H
#define NETWORK_SERVICE_ETHERNET_CONFIG_H

#include <string>

namespace network_service {

struct EthernetConfig {
    std::string iface = "eth0";
    std::string method = "dhcp";
    std::string ip4;
    std::string netmask4;
    std::string gateway4;
    std::string dns4;
    int route_metric = 10;
    bool dns_enabled = true;
};

std::string ethernet_config_path(const std::string &config_dir);
EthernetConfig load_ethernet_config(const std::string &config_dir, const std::string &iface);
bool save_ethernet_config(const std::string &config_dir, const EthernetConfig &config);
std::string ethernet_config_to_json(const EthernetConfig &config);

} // namespace network_service

#endif // NETWORK_SERVICE_ETHERNET_CONFIG_H
