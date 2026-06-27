#include "platform/interface_snapshot.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/types.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#ifndef NETWORK_SERVICE_VERSION
#define NETWORK_SERVICE_VERSION "0.1.0"
#endif

namespace network_service {

namespace {

static std::string json_escape(const std::string &value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (char ch : value) {
        switch (ch) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out += ch; break;
        }
    }
    return out;
}

static std::string sockaddr_to_ipv4(const sockaddr *addr) {
    if (!addr || addr->sa_family != AF_INET) return {};
    char buffer[INET_ADDRSTRLEN] = {0};
    const auto *in = reinterpret_cast<const sockaddr_in *>(addr);
    if (!inet_ntop(AF_INET, &in->sin_addr, buffer, sizeof(buffer))) {
        return {};
    }
    return buffer;
}

static bool read_file_trimmed(const std::string &path, std::string &out) {
    std::ifstream f(path);
    if (!f) return false;
    std::getline(f, out);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ' || out.back() == '\t')) {
        out.pop_back();
    }
    return true;
}

static bool read_carrier(const std::string &iface) {
#ifdef __linux__
    std::string value;
    if (!read_file_trimmed("/sys/class/net/" + iface + "/carrier", value)) {
        return false;
    }
    return value == "1";
#else
    (void)iface;
    return false;
#endif
}

static void populate_interface_addresses(InterfaceSnapshot &iface) {
    struct ifaddrs *ifaddr = nullptr;
    if (getifaddrs(&ifaddr) != 0) return;

    for (struct ifaddrs *ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_name || iface.iface != ifa->ifa_name) continue;
        iface.exists = true;
        iface.enabled = (ifa->ifa_flags & IFF_UP) != 0;
        iface.carrier_up = iface.carrier_up || ((ifa->ifa_flags & IFF_RUNNING) != 0);
        if (ifa->ifa_addr && ifa->ifa_addr->sa_family == AF_INET) {
            std::string ip = sockaddr_to_ipv4(ifa->ifa_addr);
            if (!ip.empty()) {
                iface.has_ip = true;
                iface.ip4 = ip;
            }
            std::string mask = sockaddr_to_ipv4(ifa->ifa_netmask);
            if (!mask.empty()) {
                iface.netmask4 = mask;
            }
        }
    }

    freeifaddrs(ifaddr);

#ifdef __linux__
    if (iface.exists) {
        iface.carrier_up = read_carrier(iface.iface);
    }
#endif
}

#ifdef __linux__
static std::string hex_gateway_to_ipv4(const std::string &hex) {
    if (hex.size() < 8) return {};
    unsigned long raw = 0;
    if (sscanf(hex.c_str(), "%lx", &raw) != 1) return {};
    struct in_addr addr;
    addr.s_addr = static_cast<in_addr_t>(raw);
    char buffer[INET_ADDRSTRLEN] = {0};
    if (!inet_ntop(AF_INET, &addr, buffer, sizeof(buffer))) return {};
    return buffer;
}

static void populate_default_routes(NetworkSnapshot &snapshot) {
    std::ifstream f("/proc/net/route");
    if (!f) return;

    std::string line;
    std::getline(f, line); // header
    while (std::getline(f, line)) {
        std::istringstream iss(line);
        std::string iface;
        std::string destination;
        std::string gateway;
        std::string flags;
        std::string refcnt;
        std::string use;
        std::string metric;
        if (!(iss >> iface >> destination >> gateway >> flags >> refcnt >> use >> metric)) {
            continue;
        }
        if (destination != "00000000") continue;

        InterfaceSnapshot *target = nullptr;
        if (iface == snapshot.eth.iface) target = &snapshot.eth;
        if (iface == snapshot.wifi.iface) target = &snapshot.wifi;
        if (!target) continue;

        target->has_default_route = true;
        target->gateway4 = hex_gateway_to_ipv4(gateway);
        target->route_metric = atoi(metric.c_str());
    }
}
#endif

static std::string read_first_dns() {
    std::ifstream f("/etc/resolv.conf");
    if (!f) return {};
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream iss(line);
        std::string key;
        std::string value;
        if ((iss >> key >> value) && key == "nameserver") {
            return value;
        }
    }
    return {};
}

static const char *route_policy_to_string(RoutePolicy policy) {
    switch (policy) {
    case RoutePolicy::WifiPreferred: return "wifi_preferred";
    case RoutePolicy::WifiOnly: return "wifi_only";
    case RoutePolicy::ManualMetric: return "manual_metric";
    case RoutePolicy::EthernetPreferred:
    default: return "ethernet_preferred";
    }
}

static const char *dns_policy_to_string(DnsPolicy policy) {
    switch (policy) {
    case DnsPolicy::Append: return "append";
    case DnsPolicy::Disabled: return "disabled";
    case DnsPolicy::Overwrite:
    default: return "overwrite";
    }
}

static std::string iface_to_json(const InterfaceSnapshot &iface) {
    std::ostringstream os;
    os << "{"
       << "\"iface\":\"" << json_escape(iface.iface) << "\",";
    os << "\"exists\":" << (iface.exists ? "true" : "false") << ",";
    os << "\"carrier_up\":" << (iface.carrier_up ? "true" : "false") << ",";
    os << "\"enabled\":" << (iface.enabled ? "true" : "false") << ",";
    os << "\"connected\":" << (iface.connected ? "true" : "false") << ",";
    os << "\"has_ip\":" << (iface.has_ip ? "true" : "false") << ",";
    os << "\"ip4\":\"" << json_escape(iface.ip4) << "\",";
    os << "\"netmask4\":\"" << json_escape(iface.netmask4) << "\",";
    os << "\"has_default_route\":" << (iface.has_default_route ? "true" : "false") << ",";
    os << "\"gateway4\":\"" << json_escape(iface.gateway4) << "\",";
    os << "\"route_metric\":" << iface.route_metric << ",";
    os << "\"ssid\":\"" << json_escape(iface.ssid) << "\",";
    os << "\"signal_dbm\":" << iface.signal_dbm << ",";
    os << "\"signal_bars\":" << iface.signal_bars;
    os << "}";
    return os.str();
}

} // namespace

NetworkSnapshot read_live_snapshot(const char *eth_iface, const char *wifi_iface) {
    NetworkSnapshot snapshot;
    snapshot.eth.iface = eth_iface && eth_iface[0] ? eth_iface : "eth0";
    snapshot.wifi.iface = wifi_iface && wifi_iface[0] ? wifi_iface : "wlan0";

    populate_interface_addresses(snapshot.eth);
    populate_interface_addresses(snapshot.wifi);

#ifdef __linux__
    populate_default_routes(snapshot);
#endif

    snapshot.wifi.connected = snapshot.wifi.has_ip;
    snapshot.eth.connected = snapshot.eth.carrier_up && snapshot.eth.has_ip;
    snapshot.dns4 = read_first_dns();
    snapshot.dns_available = !snapshot.dns4.empty();

    if (snapshot.eth.has_default_route) {
        snapshot.primary_iface = snapshot.eth.iface;
    }
    if (snapshot.wifi.has_default_route &&
        (!snapshot.eth.has_default_route ||
         snapshot.wifi.route_metric < snapshot.eth.route_metric)) {
        snapshot.primary_iface = snapshot.wifi.iface;
    }

    snapshot.online = !snapshot.primary_iface.empty() && snapshot.dns_available;
    return snapshot;
}

std::string snapshot_to_json(const NetworkSnapshot &snapshot) {
    std::ostringstream os;
    os << "{"
       << "\"version\":\"" << NETWORK_SERVICE_VERSION << "\",";
    os << "\"primary_iface\":\"" << json_escape(snapshot.primary_iface) << "\",";
    os << "\"online\":" << (snapshot.online ? "true" : "false") << ",";
    os << "\"dns_available\":" << (snapshot.dns_available ? "true" : "false") << ",";
    os << "\"dns4\":\"" << json_escape(snapshot.dns4) << "\",";
    os << "\"route_policy\":\"" << route_policy_to_string(snapshot.route_policy) << "\",";
    os << "\"dns_policy\":\"" << dns_policy_to_string(snapshot.dns_policy) << "\",";
    os << "\"eth\":" << iface_to_json(snapshot.eth) << ",";
    os << "\"wifi\":" << iface_to_json(snapshot.wifi);
    os << "}";
    return os.str();
}

} // namespace network_service
