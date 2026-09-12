#ifndef NETWORK_SERVICE_DHCP_LEASE_STORE_H
#define NETWORK_SERVICE_DHCP_LEASE_STORE_H

#include <string>

namespace network_service {

enum class DhcpLeaseEvent {
    None = 0,
    Bound,
    Renew,
    Deconfig,
};

struct DhcpLeaseFact {
    DhcpLeaseEvent event = DhcpLeaseEvent::None;
    std::string iface;
    std::string generation;
    std::string ip4;
    std::string netmask4;
    std::string gateway4;
    std::string dns4;

    bool configured() const;
    std::string fingerprint() const;
};

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
