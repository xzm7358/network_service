#ifndef NETWORK_SERVICE_ETHERNET_CONFIG_H
#define NETWORK_SERVICE_ETHERNET_CONFIG_H

#include <string>

#include "network_service_contracts.h"

namespace network_service {

std::string ethernet_config_path(const std::string &config_dir);
EthernetConfig load_ethernet_config(const std::string &config_dir, const std::string &iface);
bool save_ethernet_config(const std::string &config_dir, const EthernetConfig &config);

} // namespace network_service

#endif // NETWORK_SERVICE_ETHERNET_CONFIG_H
