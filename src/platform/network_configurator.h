#ifndef NETWORK_SERVICE_NETWORK_CONFIGURATOR_H
#define NETWORK_SERVICE_NETWORK_CONFIGURATOR_H

#include <string>

namespace network_service {

class NetworkConfigurator {
public:
    static bool apply_ipv4(const std::string &iface,
                           const std::string &ip4,
                           const std::string &netmask4,
                           std::string &error);
    static bool clear_ipv4(const std::string &iface, std::string &error);

    static bool set_default_route(const std::string &iface,
                                  const std::string &gateway4,
                                  int metric,
                                  std::string &error);
    static void clear_default_route(const std::string &iface);

    static bool set_primary_dns(const std::string &dns4, std::string &error);
    static bool clear_dns(std::string &error);
};

} // namespace network_service

#endif // NETWORK_SERVICE_NETWORK_CONFIGURATOR_H
