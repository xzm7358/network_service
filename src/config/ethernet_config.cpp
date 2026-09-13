#include "config/ethernet_config.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace network_service {

namespace {

static std::string trim(const std::string &value) {
    size_t begin = 0;
    while (begin < value.size() &&
           (value[begin] == ' ' || value[begin] == '\t' || value[begin] == '\r' ||
            value[begin] == '\n')) {
        ++begin;
    }
    size_t end = value.size();
    while (end > begin &&
           (value[end - 1] == ' ' || value[end - 1] == '\t' ||
            value[end - 1] == '\r' || value[end - 1] == '\n')) {
        --end;
    }
    return value.substr(begin, end - begin);
}

static void ensure_dir(const std::string &dir) {
    if (dir.empty()) return;
    (void)mkdir(dir.c_str(), 0755);
}

static bool write_all(int fd, const std::string &data) {
    std::size_t offset = 0;
    while (offset < data.size()) {
        const ssize_t n = write(fd, data.data() + offset, data.size() - offset);
        if (n > 0) {
            offset += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

static bool fsync_directory(const std::string &dir) {
#ifdef O_DIRECTORY
    const int fd = open(dir.c_str(), O_RDONLY | O_DIRECTORY);
#else
    const int fd = open(dir.c_str(), O_RDONLY);
#endif
    if (fd < 0) return false;
    const bool ok = fsync(fd) == 0;
    (void)close(fd);
    return ok;
}

static std::string serialize_ethernet_config(const EthernetConfig &config) {
    std::ostringstream out;
    out << "iface=" << config.iface << "\n";
    out << "method=" << config.method << "\n";
    out << "ip4=" << config.ip4 << "\n";
    out << "netmask4=" << config.netmask4 << "\n";
    out << "gateway4=" << config.gateway4 << "\n";
    out << "dns4=" << config.dns4 << "\n";
    out << "route_metric=" << config.route_metric << "\n";
    out << "dns_enabled=" << (config.dns_enabled ? 1 : 0) << "\n";
    return out.str();
}

} // namespace

std::string ethernet_config_path(const std::string &config_dir) {
    std::string dir = config_dir.empty() ? "/dnake/data" : config_dir;
    if (!dir.empty() && dir.back() == '/') dir.pop_back();
    return dir + "/smart_hmi_ethernet.conf";
}

EthernetConfig load_ethernet_config(const std::string &config_dir,
                                    const std::string &iface) {
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
    if (!dir.empty() && dir.back() == '/') dir.pop_back();
    ensure_dir(dir);

    const std::string path = ethernet_config_path(config_dir);
    const std::string tmp = path + "." + std::to_string(static_cast<long>(getpid())) + ".tmp";
    const std::string data = serialize_ethernet_config(config);

    const int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;

    bool ok = write_all(fd, data);
    if (ok) ok = fsync(fd) == 0;
    if (close(fd) != 0) ok = false;

    if (!ok) {
        (void)unlink(tmp.c_str());
        return false;
    }

    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        (void)unlink(tmp.c_str());
        return false;
    }

    // Persist the directory entry as well as file data. If this fails, the file
    // is still internally complete, but durability across sudden power loss is
    // not guaranteed, so report failure to the caller.
    return fsync_directory(dir);
}

} // namespace network_service