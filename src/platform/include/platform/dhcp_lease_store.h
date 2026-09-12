#ifndef NETWORK_SERVICE_DHCP_LEASE_STORE_H
#define NETWORK_SERVICE_DHCP_LEASE_STORE_H

#include <string>

#include "network_service_contracts.h"

namespace network_service {

class DhcpLeaseStore {
public:
    static std::string path_for(const std::string &iface,
                                const std::string &generation);
    static std::string generation_path_for(const std::string &iface);

    static bool activate_generation(const std::string &iface,
                                    const std::string &generation,
                                    std::string &error);
    static bool active_generation(const std::string &iface,
                                  std::string &generation,
                                  bool &exists,
                                  std::string &error);

    static bool read(const std::string &iface,
                     DhcpLeaseFact &fact,
                     bool &exists,
                     std::string &error);
    static void clear(const std::string &iface);
};

} // namespace network_service

#endif // NETWORK_SERVICE_DHCP_LEASE_STORE_H
