#include "config/ethernet_config.h"

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdint>
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

static std::string json_escape(const std::string &value) {
    std::string out;
    out.reserve(value.size());
    for (char ch : value) {
        if (ch == '"' || ch == '\\') out.push_back('\\');
        out.push_back(ch);
    }
    return out;
}

static int netmask_to_prefix(const std::string &netmask) {
    in_addr address{};
    if (inet_pton(AF_INET, netmask.c_str(), &address) != 1) return -1;
    std::uint32_t bits = ntohl(address.s_addr);
    int prefix = 0;
    bool saw_zero = false;
    for (int bit = 31; bit >= 0; --bit) {
        const bool set = (bits & (std::uint32_t{1} << bit)) != 0;
        if (set && saw_zero) return -1;
        if (set) ++prefix;
        else saw_zero = true;
    }
    return prefix;
}

static bool is_ipv4(const std::string &value) {
    in_addr address{};
    return !value.empty() && inet_pton(AF_INET, value.c_str(), &address) == 1;
}

static std::string prefix_to_netmask(int prefix) {
    if (prefix < 0 || prefix > 32) return {};
    const std::uint32_t bits = prefix == 0 ? 0 : 0xffffffffU << (32 - prefix);
    in_addr address{};
    address.s_addr = htonl(bits);
    char buffer[INET_ADDRSTRLEN] = {0};
    if (!inet_ntop(AF_INET, &address, buffer, sizeof(buffer))) return {};
    return buffer;
}

static std::string serialize_ethernet_config(const EthernetConfig &config) {
    std::ostringstream out;
    if (config.method != "static") {
        out << "{\"mode\":\"dhcp\"}\n";
        return out.str();
    }

    const int prefix = netmask_to_prefix(config.netmask4);
    out << "{\"mode\":\"static\","
        << "\"address\":\"" << json_escape(config.ip4) << "\","
        << "\"prefix\":" << prefix << ','
        << "\"gateway\":\"" << json_escape(config.gateway4) << "\","
        << "\"dns\":[";
    if (config.dns_enabled && !config.dns4.empty()) {
        out << '"' << json_escape(config.dns4) << '"';
    }
    out << "]}\n";
    return out.str();
}

static std::string config_directory(const std::string &config_dir) {
    std::string dir = config_dir.empty() ? "/data" : config_dir;
    if (!dir.empty() && dir.back() == '/') dir.pop_back();
    return dir;
}

static std::string authority_directory(const std::string &config_dir) {
    return config_directory(config_dir) + "/network-service";
}

static std::string legacy_config_path(const std::string &config_dir) {
    return config_directory(config_dir) + "/smart_hmi_ethernet.conf";
}

static bool extract_json_string(const std::string &json,
                                const std::string &key,
                                std::string &value) {
    const std::string needle = "\"" + key + "\"";
    std::size_t pos = json.find(needle);
    if (pos == std::string::npos) return false;
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return false;
    pos = json.find('"', pos + 1);
    if (pos == std::string::npos) return false;
    ++pos;

    value.clear();
    bool escaped = false;
    for (; pos < json.size(); ++pos) {
        const char ch = json[pos];
        if (escaped) {
            value.push_back(ch);
            escaped = false;
        } else if (ch == '\\') {
            escaped = true;
        } else if (ch == '"') {
            return true;
        } else {
            value.push_back(ch);
        }
    }
    return false;
}

static bool extract_json_int(const std::string &json,
                             const std::string &key,
                             int &value) {
    const std::string needle = "\"" + key + "\"";
    std::size_t pos = json.find(needle);
    if (pos == std::string::npos) return false;
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return false;
    ++pos;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
    char *end = nullptr;
    const long parsed = std::strtol(json.c_str() + pos, &end, 10);
    if (end == json.c_str() + pos || parsed < 0 || parsed > 32) return false;
    value = static_cast<int>(parsed);
    return true;
}

static bool extract_first_dns(const std::string &json, std::string &dns) {
    const std::string needle = "\"dns\"";
    std::size_t pos = json.find(needle);
    if (pos == std::string::npos) return false;
    pos = json.find('[', pos + needle.size());
    if (pos == std::string::npos) return false;
    const std::size_t end = json.find(']', pos + 1);
    if (end == std::string::npos) return false;
    const std::size_t quote = json.find('"', pos + 1);
    if (quote == std::string::npos || quote > end) {
        dns.clear();
        return true;
    }
    const std::string array = json.substr(quote, end - quote + 1);
    return extract_json_string("{\"dns\":" + array + "}", "dns", dns);
}

static bool parse_json_config(const std::string &json,
                              const std::string &iface,
                              EthernetConfig &config) {
    std::string mode;
    if (!extract_json_string(json, "mode", mode)) return false;

    config = EthernetConfig{};
    config.iface = iface.empty() ? "eth0" : iface;
    config.route_metric = 10;
    if (mode == "dhcp") {
        config.method = "dhcp";
        config.dns_enabled = true;
        return true;
    }
    if (mode != "static") return false;

    int prefix = -1;
    std::string dns;
    if (!extract_json_string(json, "address", config.ip4) ||
        !extract_json_int(json, "prefix", prefix) ||
        !extract_json_string(json, "gateway", config.gateway4) ||
        !extract_first_dns(json, dns)) {
        return false;
    }
    config.netmask4 = prefix_to_netmask(prefix);
    if (!is_ipv4(config.ip4) || config.netmask4.empty() ||
        !is_ipv4(config.gateway4) || (!dns.empty() && !is_ipv4(dns))) {
        return false;
    }
    config.method = "static";
    config.dns4 = dns;
    config.dns_enabled = !dns.empty();
    return true;
}

static bool load_json_config(const std::string &path,
                             const std::string &iface,
                             EthernetConfig &config) {
    std::ifstream input(path);
    if (!input) return false;
    std::ostringstream content;
    content << input.rdbuf();
    return parse_json_config(content.str(), iface, config);
}

static bool load_legacy_config(const std::string &path,
                               const std::string &iface,
                               EthernetConfig &config) {
    std::ifstream input(path);
    if (!input) return false;

    config = EthernetConfig{};
    config.iface = iface.empty() ? "eth0" : iface;
    std::string line;
    while (std::getline(input, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = trim(line.substr(0, eq));
        const std::string value = trim(line.substr(eq + 1));
        if (key == "iface") config.iface = value;
        else if (key == "method") config.method = value;
        else if (key == "ip4") config.ip4 = value;
        else if (key == "netmask4") config.netmask4 = value;
        else if (key == "gateway4") config.gateway4 = value;
        else if (key == "dns4") config.dns4 = value;
        else if (key == "route_metric") config.route_metric = std::atoi(value.c_str());
        else if (key == "dns_enabled") config.dns_enabled = value != "0";
    }
    if (config.iface.empty()) config.iface = iface.empty() ? "eth0" : iface;
    if (config.method != "static") config.method = "dhcp";
    return true;
}

} // namespace

std::string ethernet_config_path(const std::string &config_dir) {
    return authority_directory(config_dir) + "/ethernet.json";
}

EthernetConfig load_ethernet_config(const std::string &config_dir,
                                    const std::string &iface) {
    EthernetConfig config;
    std::string ignored;
    (void)load_ethernet_config(config_dir, iface, config, ignored);
    return config;
}

bool load_ethernet_config(const std::string &config_dir,
                          const std::string &iface,
                          EthernetConfig &config,
                          std::string &error) {
    error.clear();
    config = EthernetConfig{};
    config.iface = iface.empty() ? "eth0" : iface;
    const std::string requested_iface = config.iface;

    const std::string authoritative = ethernet_config_path(config_dir);
    std::ifstream probe(authoritative);
    if (probe.good()) {
        probe.close();
        if (load_json_config(authoritative, requested_iface, config)) return true;
        config = EthernetConfig{};
        config.iface = requested_iface;
        error = "invalid Ethernet configuration: " + authoritative;
        return false;
    }

    EthernetConfig legacy;
    if (load_legacy_config(legacy_config_path(config_dir), requested_iface, legacy)) {
        if (!save_ethernet_config(config_dir, legacy)) {
            error = "failed to migrate legacy Ethernet configuration";
            return false;
        }
        config = legacy;
        return true;
    }
    return true;
}

bool stage_ethernet_config(const std::string &config_dir,
                           const EthernetConfig &config,
                           EthernetConfigStage &stage,
                           std::string &error) {
    discard_ethernet_config(stage);
    error.clear();

    if (config.method != "dhcp" && config.method != "static") {
        error = "Ethernet mode must be dhcp or static";
        return false;
    }
    if (config.method == "static" &&
        (!is_ipv4(config.ip4) || netmask_to_prefix(config.netmask4) < 0 ||
         !is_ipv4(config.gateway4) ||
         (config.dns_enabled && !config.dns4.empty() && !is_ipv4(config.dns4)))) {
        error = "invalid static Ethernet configuration";
        return false;
    }

    const std::string root = config_directory(config_dir);
    const std::string dir = authority_directory(config_dir);
    if (!ensure_dir(root, error) || !ensure_dir(dir, error)) return false;

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
