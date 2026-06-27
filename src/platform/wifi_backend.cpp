#include "platform/wifi_backend.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace network_service {

namespace {

static bool is_safe_iface(const std::string &value) {
    if (value.empty() || value.size() > 15) return false;
    for (char ch : value) {
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.')) {
            return false;
        }
    }
    return true;
}

static std::string shell_quote(const std::string &value) {
    std::string out = "'";
    for (char ch : value) {
        if (ch == '\'') {
            out += "'\\''";
        } else {
            out += ch;
        }
    }
    out += "'";
    return out;
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

static bool run_command(const std::string &cmd, std::string &error) {
    int rc = system(cmd.c_str());
    if (rc != 0) {
        std::ostringstream os;
        os << "command failed rc=" << rc << ": " << cmd;
        error = os.str();
        return false;
    }
    return true;
}

static std::string read_command(const std::string &cmd, std::string &error) {
    FILE *fp = popen(cmd.c_str(), "r");
    if (!fp) {
        error = "popen failed";
        return {};
    }
    std::string output;
    char buffer[512];
    while (fgets(buffer, sizeof(buffer), fp)) {
        output += buffer;
    }
    int rc = pclose(fp);
    if (rc != 0 && output.empty()) {
        std::ostringstream os;
        os << "command failed rc=" << rc << ": " << cmd;
        error = os.str();
    }
    return output;
}

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

static std::vector<std::string> split_tab_line(const std::string &line) {
    std::vector<std::string> fields;
    size_t start = 0;
    while (start <= line.size()) {
        size_t tab = line.find('\t', start);
        if (tab == std::string::npos) {
            fields.push_back(line.substr(start));
            break;
        }
        fields.push_back(line.substr(start, tab - start));
        start = tab + 1;
    }
    return fields;
}

static std::string dhcp_pidfile(const std::string &iface) {
    return "/tmp/smart_hmi_udhcpc_" + iface + ".pid";
}

static std::string dhcp_script_path() {
    return "/tmp/smart_hmi_udhcpc_wifi_network_service.script";
}

static void stop_dhcp_for_iface(const std::string &iface) {
    std::ifstream f(dhcp_pidfile(iface));
    int pid = -1;
    if (f >> pid) {
        if (pid > 1) {
            char cmd[128];
            snprintf(cmd, sizeof(cmd), "kill %d 2>/dev/null", pid);
            (void)system(cmd);
        }
    }
    std::string rm = "rm -f " + dhcp_pidfile(iface);
    (void)system(rm.c_str());
}

static bool ensure_dhcp_script(std::string &error) {
    const std::string path = dhcp_script_path();
    std::ofstream f(path, std::ios::out | std::ios::trunc);
    if (!f) {
        error = "failed to write Wi-Fi udhcpc script";
        return false;
    }
    f << "#!/bin/sh\n"
      << "case \"$1\" in\n"
      << "  deconfig)\n"
      << "    ifconfig \"$interface\" 0.0.0.0 2>/dev/null\n"
      << "    ;;\n"
      << "  bound|renew)\n"
      << "    ifconfig \"$interface\" \"$ip\" netmask \"$subnet\" up\n"
      << "    if [ -n \"$router\" ]; then\n"
      << "      route del default dev \"$interface\" 2>/dev/null\n"
      << "      for r in $router; do route add default gw \"$r\" dev \"$interface\" metric 20 2>/dev/null; break; done\n"
      << "    fi\n"
      << "    if [ -n \"$dns\" ]; then\n"
      << "      : > /etc/resolv.conf\n"
      << "      for d in $dns; do echo nameserver \"$d\" >> /etc/resolv.conf; done\n"
      << "    fi\n"
      << "    ;;\n"
      << "esac\n"
      << "exit 0\n";
    f.close();
    chmod(path.c_str(), 0755);
    return true;
}

static bool start_wifi_dhcp(const std::string &iface, std::string &error) {
    if (!ensure_dhcp_script(error)) return false;
    stop_dhcp_for_iface(iface);
    std::ostringstream os;
    os << "udhcpc -i " << iface
       << " -t 15 -n -p " << shell_quote(dhcp_pidfile(iface))
       << " -s " << shell_quote(dhcp_script_path()) << " &";
    return run_command(os.str(), error);
}

static bool wpa_cli_ok(const std::string &iface, const std::string &args, std::string &error) {
    if (!is_safe_iface(iface)) {
        error = "invalid wifi iface";
        return false;
    }
    std::string output = read_command("wpa_cli -i " + iface + " " + args + " 2>/dev/null", error);
    std::string normalized = trim(output);
    if (normalized == "OK" || normalized.find("OK\n") != std::string::npos) {
        return true;
    }
    if (error.empty()) {
        error = "wpa_cli returned: " + normalized;
    }
    return false;
}

static int parse_network_id(const std::string &value) {
    char *end = nullptr;
    long id = strtol(value.c_str(), &end, 10);
    if (end == value.c_str() || id < 0 || id > 4096) {
        return -1;
    }
    return static_cast<int>(id);
}

static int wpa_add_network(const std::string &iface, std::string &error) {
    std::string output = trim(read_command("wpa_cli -i " + iface + " add_network 2>/dev/null", error));
    if (output.empty()) return -1;
    int id = parse_network_id(output);
    if (id < 0 && error.empty()) {
        error = "invalid add_network result: " + output;
    }
    return id;
}

static void parse_saved_flags(const std::string &flags, WifiSavedNetwork &record) {
    record.is_current = flags.find("CURRENT") != std::string::npos;
    record.is_disabled = flags.find("DISABLED") != std::string::npos;
    record.is_temp_disabled = flags.find("TEMP-DISABLED") != std::string::npos;
    record.autoconnect = !record.is_disabled;
}

static std::vector<WifiSavedNetwork> parse_saved_networks(const std::string &output) {
    std::vector<WifiSavedNetwork> records;
    std::istringstream input(output);
    std::string line;
    bool header = true;
    while (std::getline(input, line)) {
        line = trim(line);
        if (line.empty()) continue;
        if (header) {
            header = false;
            continue;
        }
        std::vector<std::string> fields = split_tab_line(line);
        if (fields.size() < 2) continue;
        WifiSavedNetwork record;
        record.network_id = parse_network_id(fields[0]);
        record.ssid = fields[1];
        record.bssid = fields.size() >= 3 ? fields[2] : "";
        record.flags = fields.size() >= 4 ? fields[3] : "";
        parse_saved_flags(record.flags, record);
        if (record.network_id >= 0) {
            records.push_back(record);
        }
    }
    return records;
}

static int find_saved_network_id(const std::string &iface, const std::string &ssid, std::string &error) {
    std::string output = read_command("wpa_cli -i " + iface + " list_networks 2>/dev/null", error);
    std::vector<WifiSavedNetwork> records = parse_saved_networks(output);
    for (const auto &record : records) {
        if (record.ssid == ssid) {
            return record.network_id;
        }
    }
    error = "saved network not found: " + ssid;
    return -1;
}

} // namespace

bool wifi_start_dhcp(const std::string &iface, std::string &error) {
    if (!is_safe_iface(iface)) {
        error = "invalid wifi iface";
        return false;
    }
    return start_wifi_dhcp(iface, error);
}

bool wifi_set_enabled(const std::string &iface, bool enabled, std::string &error) {
    if (!is_safe_iface(iface)) {
        error = "invalid wifi iface";
        return false;
    }
    if (enabled) {
        if (!run_command("ifconfig " + iface + " up", error)) return false;
        (void)wpa_cli_ok(iface, "reconnect", error);
        return true;
    }
    (void)wifi_disconnect(iface, error);
    return run_command("ifconfig " + iface + " down", error);
}

bool wifi_disconnect(const std::string &iface, std::string &error) {
    if (!is_safe_iface(iface)) {
        error = "invalid wifi iface";
        return false;
    }
    stop_dhcp_for_iface(iface);
    (void)wpa_cli_ok(iface, "disconnect", error);
    (void)system(("route del default dev " + iface + " 2>/dev/null").c_str());
    (void)system(("ifconfig " + iface + " 0.0.0.0 2>/dev/null").c_str());
    return true;
}

bool wifi_connect(const std::string &iface,
                  const std::string &ssid,
                  const std::string &password,
                  std::string &error) {
    if (!is_safe_iface(iface)) {
        error = "invalid wifi iface";
        return false;
    }
    if (ssid.empty()) {
        error = "ssid is required";
        return false;
    }
    if (!run_command("ifconfig " + iface + " up", error)) return false;

    int id = wpa_add_network(iface, error);
    if (id < 0) return false;

    std::string quoted_ssid = "\\\"" + ssid + "\\\"";
    if (!wpa_cli_ok(iface, "set_network " + std::to_string(id) + " ssid " + shell_quote(quoted_ssid), error)) {
        return false;
    }

    if (password.empty()) {
        if (!wpa_cli_ok(iface, "set_network " + std::to_string(id) + " key_mgmt NONE", error)) {
            return false;
        }
    } else {
        std::string quoted_psk = "\\\"" + password + "\\\"";
        if (!wpa_cli_ok(iface, "set_network " + std::to_string(id) + " psk " + shell_quote(quoted_psk), error)) {
            return false;
        }
    }

    (void)wpa_cli_ok(iface, "disable_network all", error);
    if (!wpa_cli_ok(iface, "enable_network " + std::to_string(id), error)) return false;
    if (!wpa_cli_ok(iface, "select_network " + std::to_string(id), error)) return false;
    (void)wpa_cli_ok(iface, "save_config", error);

    return true;
}

bool wifi_connect_saved(const std::string &iface, const std::string &ssid, std::string &error) {
    if (!is_safe_iface(iface)) {
        error = "invalid wifi iface";
        return false;
    }
    if (ssid.empty()) {
        error = "ssid is required";
        return false;
    }
    if (!run_command("ifconfig " + iface + " up", error)) return false;
    int id = find_saved_network_id(iface, ssid, error);
    if (id < 0) return false;
    (void)wpa_cli_ok(iface, "disable_network all", error);
    if (!wpa_cli_ok(iface, "enable_network " + std::to_string(id), error)) return false;
    if (!wpa_cli_ok(iface, "select_network " + std::to_string(id), error)) return false;
    return true;
}

bool wifi_forget_saved(const std::string &iface, const std::string &ssid, std::string &error) {
    if (!is_safe_iface(iface)) {
        error = "invalid wifi iface";
        return false;
    }
    int id = find_saved_network_id(iface, ssid, error);
    if (id < 0) return false;
    if (!wpa_cli_ok(iface, "remove_network " + std::to_string(id), error)) return false;
    return wpa_cli_ok(iface, "save_config", error);
}

bool wifi_set_autoconnect(const std::string &iface,
                          const std::string &ssid,
                          bool enabled,
                          std::string &error) {
    if (!is_safe_iface(iface)) {
        error = "invalid wifi iface";
        return false;
    }
    int id = find_saved_network_id(iface, ssid, error);
    if (id < 0) return false;
    if (!wpa_cli_ok(iface,
                    std::string(enabled ? "enable_network " : "disable_network ") + std::to_string(id),
                    error)) {
        return false;
    }
    return wpa_cli_ok(iface, "save_config", error);
}

std::vector<WifiApRecord> wifi_scan(const std::string &iface, std::string &error) {
    std::vector<WifiApRecord> records;
    if (!is_safe_iface(iface)) {
        error = "invalid wifi iface";
        return records;
    }
    if (!run_command("ifconfig " + iface + " up", error)) {
        return records;
    }
    (void)wpa_cli_ok(iface, "scan", error);
    usleep(1200 * 1000);

    std::string output = read_command("wpa_cli -i " + iface + " scan_results 2>/dev/null", error);
    std::istringstream input(output);
    std::string line;
    bool header = true;
    while (std::getline(input, line)) {
        line = trim(line);
        if (line.empty()) continue;
        if (header) {
            header = false;
            continue;
        }
        std::vector<std::string> fields = split_tab_line(line);
        if (fields.size() < 5) continue;
        WifiApRecord record;
        record.bssid = fields[0];
        record.frequency = atoi(fields[1].c_str());
        record.signal_dbm = atoi(fields[2].c_str());
        record.flags = fields[3];
        record.ssid = fields[4];
        records.push_back(record);
    }
    return records;
}

std::vector<WifiSavedNetwork> wifi_list_saved(const std::string &iface, std::string &error) {
    if (!is_safe_iface(iface)) {
        error = "invalid wifi iface";
        return {};
    }
    std::string output = read_command("wpa_cli -i " + iface + " list_networks 2>/dev/null", error);
    return parse_saved_networks(output);
}

std::string wifi_scan_to_json(const std::vector<WifiApRecord> &records) {
    std::ostringstream os;
    os << "{\"count\":" << records.size() << ",\"aps\":[";
    for (size_t i = 0; i < records.size(); ++i) {
        const WifiApRecord &record = records[i];
        if (i > 0) os << ",";
        os << "{"
           << "\"bssid\":\"" << json_escape(record.bssid) << "\","
           << "\"frequency\":" << record.frequency << ","
           << "\"signal\":" << record.signal_dbm << ","
           << "\"flags\":\"" << json_escape(record.flags) << "\","
           << "\"ssid\":\"" << json_escape(record.ssid) << "\""
           << "}";
    }
    os << "]}";
    return os.str();
}

std::string wifi_saved_to_json(const std::vector<WifiSavedNetwork> &records) {
    std::ostringstream os;
    os << "{\"count\":" << records.size() << ",\"saved\":[";
    for (size_t i = 0; i < records.size(); ++i) {
        const WifiSavedNetwork &record = records[i];
        if (i > 0) os << ",";
        os << "{"
           << "\"network_id\":" << record.network_id << ","
           << "\"ssid\":\"" << json_escape(record.ssid) << "\","
           << "\"bssid\":\"" << json_escape(record.bssid) << "\","
           << "\"flags\":\"" << json_escape(record.flags) << "\","
           << "\"current\":" << (record.is_current ? "true" : "false") << ","
           << "\"disabled\":" << (record.is_disabled ? "true" : "false") << ","
           << "\"temp_disabled\":" << (record.is_temp_disabled ? "true" : "false") << ","
           << "\"autoconnect\":" << (record.autoconnect ? "true" : "false")
           << "}";
    }
    os << "]}";
    return os.str();
}

} // namespace network_service
