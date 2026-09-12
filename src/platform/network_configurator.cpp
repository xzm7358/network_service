#include "platform/network_configurator.h"

#include <arpa/inet.h>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

namespace network_service {
namespace {

constexpr const char *kDnsOwnerMarker = "# managed-by-network-service";

bool is_safe_iface(const std::string &value) {
    if (value.empty() || value.size() > 15) return false;
    for (char ch : value) {
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.')) {
            return false;
        }
    }
    return true;
}

bool is_ipv4(const std::string &value) {
    if (value.empty()) return false;
    in_addr addr{};
    return inet_pton(AF_INET, value.c_str(), &addr) == 1;
}

bool run_command(const std::string &cmd, std::string &error) {
    const int rc = system(cmd.c_str());
    if (rc == 0) return true;
    std::ostringstream os;
    os << "command failed rc=" << rc << ": " << cmd;
    error = os.str();
    return false;
}

bool dns_file_owned_by_network_service() {
    std::ifstream input("/etc/resolv.conf");
    if (!input) return false;
    std::string first;
    std::getline(input, first);
    return first == kDnsOwnerMarker;
}

} // namespace

bool NetworkConfigurator::apply_ipv4(const std::string &iface,
                                     const std::string &ip4,
                                     const std::string &netmask4,
                                     std::string &error) {
    error.clear();
    if (!is_safe_iface(iface) || !is_ipv4(ip4) || !is_ipv4(netmask4)) {
        error = "invalid IPv4 interface configuration";
        return false;
    }

    char cmd[256];
    std::snprintf(cmd, sizeof(cmd), "ifconfig %s %s netmask %s up",
                  iface.c_str(), ip4.c_str(), netmask4.c_str());
    return run_command(cmd, error);
}

bool NetworkConfigurator::clear_ipv4(const std::string &iface, std::string &error) {
    error.clear();
    if (!is_safe_iface(iface)) {
        error = "invalid interface";
        return false;
    }
    return run_command("ifconfig " + iface + " 0.0.0.0", error);
}

bool NetworkConfigurator::set_default_route(const std::string &iface,
                                            const std::string &gateway4,
                                            int metric,
                                            std::string &error) {
    error.clear();
    if (!is_safe_iface(iface) || !is_ipv4(gateway4) || metric < 0 || metric > 65535) {
        error = "invalid default route configuration";
        return false;
    }

    clear_default_route(iface);

    char cmd[256];
    std::snprintf(cmd, sizeof(cmd), "route add default gw %s dev %s metric %d",
                  gateway4.c_str(), iface.c_str(), metric);
    return run_command(cmd, error);
}

void NetworkConfigurator::clear_default_route(const std::string &iface) {
    if (!is_safe_iface(iface)) return;
    (void)system(("route del default dev " + iface + " 2>/dev/null").c_str());
}

bool NetworkConfigurator::set_primary_dns(const std::string &dns4, std::string &error) {
    error.clear();
    if (!is_ipv4(dns4)) {
        error = "invalid DNS IPv4 address";
        return false;
    }
    std::ofstream out("/etc/resolv.conf", std::ios::out | std::ios::trunc);
    if (!out) {
        error = "failed to open /etc/resolv.conf";
        return false;
    }
    out << kDnsOwnerMarker << '\n'
        << "nameserver " << dns4 << '\n';
    out.close();
    if (!out) {
        error = "failed to write /etc/resolv.conf";
        return false;
    }
    return true;
}

bool NetworkConfigurator::clear_dns(std::string &error) {
    error.clear();

    // DNS may also be owned by a protected/external Ethernet management path.
    // Never truncate a resolver file after another owner has replaced ours.
    if (!dns_file_owned_by_network_service()) return true;

    std::ofstream out("/etc/resolv.conf", std::ios::out | std::ios::trunc);
    if (!out) {
        error = "failed to open /etc/resolv.conf";
        return false;
    }
    out.close();
    if (!out) {
        error = "failed to clear /etc/resolv.conf";
        return false;
    }
    return true;
}

} // namespace network_service
