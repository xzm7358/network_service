#ifndef NETWORK_SERVICE_ETHERNET_APPLY_H
#define NETWORK_SERVICE_ETHERNET_APPLY_H

#include <string>

#include "config/ethernet_config.h"

namespace network_service {

bool apply_ethernet_static(const EthernetConfig &config, std::string &error);
bool apply_ethernet_dhcp(const EthernetConfig &config, std::string &error);

} // namespace network_service

#endif // NETWORK_SERVICE_ETHERNET_APPLY_H
