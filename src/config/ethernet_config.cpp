#include "config/ethernet_config.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <sys/types.h>

namespace network_service {

namespace {

static std::string trim(const std::string &value) {
    size_t begin = 0;
    while (begin < value.size() && (value[begin] == ' ' || value[begin] == '\t' || value[begin] == '\r' || value[begin] == '\n')) {
        ++begin;
    }
    size_t end = value.size();
    while (end > begin && (value[end - 1] == ' ' || value[end - 1] == '\t' || value[end - 1] == '\r' || value[end - 1] == '\n')) {
        --end;
    }
    return value.substr(begin, end - begin);
}

static void ensure_dir(const std::string &dir) {
    if (dir.empty()) return;
    (void)mkdir(dir.c_str(), 0755);
}

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

} // namespace

std::string ethernet_config_path(const std::string &config_dir) {
    std::string dir = config_dir.empty() ? "/dnake/data" : config_dir;
    if (!dir.empty() && dir.back() == '/') {
        dir.pop_back();
    }
    return dir + "/smart_hmi_ethernet.conf";
}

EthernetConfig load_ethernet_config(const std::string &config_dir, const std::string &iface) {
    EthernetConfig config;
    config.iface = iface.empty() ? "eth0" : iface;

    std::ifstream f(ethernet_config_path(config_dir));
    if (!f) return config;

    std::string line;
    while (std::getline(f, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(line.substr(0, eq));
        std::string value = trim(line.substr(eq + 1));
        if (key == "iface") config.iface = value;
        else if (key == "method") config.method = value;
        else if (key == "ip4") config.ip4 = value;
        else if (key == "netmask4") config.netmask4 = value;
        else if (key == "gateway4") config.gateway4 = value;
        else if (key == "dns4") config.dns4 = value;
        else if (key == "route_metric") config.route_metric = atoi(value.c_str());
        else if (key == "dns_enabled") config.dns_enabled = value != "0";
    }
    if (config.iface.empty()) config.iface = iface.empty() ? "eth0" : iface;
    if (config.method != "static") config.method = "dhcp";
    return config;
}

bool save_ethernet_config(const std::string &config_dir, const EthernetConfig &config) {
    std::string dir = config_dir.empty() ? "/dnake/data" : config_dir;
    ensure_dir(dir);
    std::ofstream f(ethernet_config_path(config_dir), std::ios::out | std::ios::trunc);
    if (!f) return false;
    f << "iface=" << config.iface << "\n";
    f << "method=" << config.method << "\n";
    f << "ip4=" << config.ip4 << "\n";
    f << "netmask4=" << config.netmask4 << "\n";
    f << "gateway4=" << config.gateway4 << "\n";
    f << "dns4=" << config.dns4 << "\n";
    f << "route_metric=" << config.route_metric << "\n";
    f << "dns_enabled=" << (config.dns_enabled ? 1 : 0) << "\n";
    return true;
}

std::string ethernet_config_to_json(const EthernetConfig &config) {
    std::ostringstream os;
    os << "{"
       << "\"iface\":\"" << json_escape(config.iface) << "\","
       << "\"method\":\"" << json_escape(config.method) << "\","
       << "\"ip4\":\"" << json_escape(config.ip4) << "\","
       << "\"netmask4\":\"" << json_escape(config.netmask4) << "\","
       << "\"gateway4\":\"" << json_escape(config.gateway4) << "\","
       << "\"dns4\":\"" << json_escape(config.dns4) << "\","
       << "\"route_metric\":" << config.route_metric << ","
       << "\"dns_enabled\":" << (config.dns_enabled ? "true" : "false")
       << "}";
    return os.str();
}

} // namespace network_service
