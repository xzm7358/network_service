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

std::string hex_gateway_to_ipv4(const std::string &hex) {
#ifdef __linux__
    if (hex.size() < 8) return {};
    unsigned long raw = 0;
    if (std::sscanf(hex.c_str(), "%lx", &raw) != 1) return {};
    in_addr addr{};
    addr.s_addr = static_cast<in_addr_t>(raw);
    char buffer[INET_ADDRSTRLEN] = {0};
    if (!inet_ntop(AF_INET, &addr, buffer, sizeof(buffer))) return {};
    return buffer;
#else
    (void)hex;
    return {};
#endif
}

bool exact_default_route_exists(const std::string &iface,
                                const std::string &gateway4,
                                int metric) {
#ifdef __linux__
    std::ifstream input("/proc/net/route");
    if (!input) return false;
    std::string line;
    std::getline(input, line);
    while (std::getline(input, line)) {
        std::istringstream row(line);
        std::string row_iface;
        std::string destination;
        std::string gateway;
        std::string flags;
        std::string refcnt;
        std::string use;
        std::string row_metric;
        if (!(row >> row_iface >> destination >> gateway >> flags >> refcnt >> use >> row_metric)) {
            continue;
        }
        if (row_iface != iface || destination != "00000000") continue;
        if (hex_gateway_to_ipv4(gateway) != gateway4) continue;
        char *end = nullptr;
        const long parsed_metric = std::strtol(row_metric.c_str(), &end, 10);
        if (end == row_metric.c_str() || *end != '\0') continue;
        if (parsed_metric == metric) return true;
    }
#else
    (void)iface;
    (void)gateway4;
    (void)metric;
#endif
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

    if (exact_default_route_exists(iface, gateway4, metric)) return true;

    // Do not clear other default routes on the interface. They may belong to a
    // different network manager. Service owns only the exact identity it asks us
    // to ensure and records that identity for later deletion.
    char cmd[256];
    std::snprintf(cmd, sizeof(cmd), "route add default gw %s dev %s metric %d",
                  gateway4.c_str(), iface.c_str(), metric);
    return run_command(cmd, error);
}

bool NetworkConfigurator::clear_default_route(const std::string &iface,
                                              const std::string &gateway4,
                                              int metric,
                                              std::string &error) {
    error.clear();
    if (!is_safe_iface(iface) || !is_ipv4(gateway4) || metric < 0 || metric > 65535) {
        error = "invalid default route identity";
        return false;
    }

    if (!exact_default_route_exists(iface, gateway4, metric)) return true;

    char cmd[256];
    std::snprintf(cmd, sizeof(cmd), "route del default gw %s dev %s metric %d",
                  gateway4.c_str(), iface.c_str(), metric);
    return run_command(cmd, error);
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
