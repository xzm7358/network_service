#include "config/ethernet_config.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace network_service {

namespace {

std::atomic<unsigned long long> g_stage_sequence{0};

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

static bool ensure_dir(const std::string &dir, std::string &error) {
    if (dir.empty()) {
        error = "ethernet config directory is empty";
        return false;
    }
    if (mkdir(dir.c_str(), 0755) == 0 || errno == EEXIST) return true;
    error = std::string("failed to create ethernet config directory: ") +
            std::strerror(errno);
    return false;
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

static std::string config_directory(const std::string &config_dir) {
    std::string dir = config_dir.empty() ? "/dnake/data" : config_dir;
    if (!dir.empty() && dir.back() == '/') dir.pop_back();
    return dir;
}

} // namespace

std::string ethernet_config_path(const std::string &config_dir) {
    return config_directory(config_dir) + "/smart_hmi_ethernet.conf";
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

bool stage_ethernet_config(const std::string &config_dir,
                           const EthernetConfig &config,
                           EthernetConfigStage &stage,
                           std::string &error) {
    discard_ethernet_config(stage);
    error.clear();

    const std::string dir = config_directory(config_dir);
    if (!ensure_dir(dir, error)) return false;

    const std::string path = ethernet_config_path(config_dir);
    const unsigned long long sequence =
        g_stage_sequence.fetch_add(1, std::memory_order_relaxed);
    const std::string tmp = path + "." + std::to_string(static_cast<long>(getpid())) +
                            "." + std::to_string(sequence) + ".stage";
    const std::string data = serialize_ethernet_config(config);

    const int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) {
        error = std::string("failed to create staged ethernet config: ") +
                std::strerror(errno);
        return false;
    }

    bool ok = write_all(fd, data);
    if (!ok) {
        error = std::string("failed to write staged ethernet config: ") +
                std::strerror(errno);
    }
    if (ok && fsync(fd) != 0) {
        ok = false;
        error = std::string("failed to fsync staged ethernet config: ") +
                std::strerror(errno);
    }
    if (close(fd) != 0 && ok) {
        ok = false;
        error = std::string("failed to close staged ethernet config: ") +
                std::strerror(errno);
    }

    if (!ok) {
        (void)unlink(tmp.c_str());
        return false;
    }

    stage.temp_path = tmp;
    stage.final_path = path;
    stage.directory = dir;
    stage.valid = true;
    return true;
}

EthernetConfigCommitResult commit_ethernet_config(EthernetConfigStage &stage,
                                                   std::string &error) {
    error.clear();
    if (!stage.valid || stage.temp_path.empty() || stage.final_path.empty() ||
        stage.directory.empty()) {
        error = "ethernet config stage is invalid";
        return EthernetConfigCommitResult::Failed;
    }

    if (std::rename(stage.temp_path.c_str(), stage.final_path.c_str()) != 0) {
        error = std::string("failed to commit ethernet config: ") +
                std::strerror(errno);
        return EthernetConfigCommitResult::Failed;
    }

    // rename() is the logical commit point. From here the final path contains the
    // complete new file. Do not pretend it was not committed if only the directory
    // durability barrier fails; callers must keep runtime aligned with this file.
    stage.temp_path.clear();
    stage.valid = false;

    if (!fsync_directory(stage.directory)) {
        error = std::string("ethernet config committed but directory fsync failed: ") +
                std::strerror(errno);
        stage.final_path.clear();
        stage.directory.clear();
        return EthernetConfigCommitResult::CommittedDurabilityUncertain;
    }

    stage.final_path.clear();
    stage.directory.clear();
    return EthernetConfigCommitResult::CommittedDurable;
}

void discard_ethernet_config(EthernetConfigStage &stage) {
    if (stage.valid && !stage.temp_path.empty()) {
        (void)unlink(stage.temp_path.c_str());
    }
    stage = EthernetConfigStage{};
}

bool save_ethernet_config(const std::string &config_dir, const EthernetConfig &config) {
    EthernetConfigStage stage;
    std::string error;
    if (!stage_ethernet_config(config_dir, config, stage, error)) return false;

    const EthernetConfigCommitResult result = commit_ethernet_config(stage, error);
    if (result == EthernetConfigCommitResult::Failed) {
        discard_ethernet_config(stage);
        return false;
    }
    return result == EthernetConfigCommitResult::CommittedDurable;
}

} // namespace network_service
