#ifndef NETWORK_SERVICE_ETHERNET_STARTUP_H
#define NETWORK_SERVICE_ETHERNET_STARTUP_H

#include <functional>
#include <string>

#include "network_service_contracts.h"

namespace network_service {

enum class EthernetDhcpAdoption {
    Absent = 0,
    Adopted,
    Conflict,
    Failed,
};

// Boundary ports for the one-shot startup convergence. Keeping filesystem,
// process discovery, and network mutation behind these ports makes startup
// policy deterministic without exposing Platform implementation types.
struct EthernetStartupOps {
    std::function<bool(EthernetConfig &, std::string &)> load_config;
    std::function<EthernetDhcpAdoption(std::string &)> try_adopt_dhcp;
    std::function<bool(std::string &)> start_dhcp;
    std::function<bool(const EthernetConfig &, std::string &)> apply_static;
};

class EthernetStartup {
public:
    explicit EthernetStartup(EthernetStartupOps ops);

    // Loads the authoritative config before any runtime mutation. A failed load
    // deliberately leaves a brownfield link untouched.
    bool converge(std::string &error);

private:
    EthernetStartupOps ops_;
};

} // namespace network_service

#endif // NETWORK_SERVICE_ETHERNET_STARTUP_H
