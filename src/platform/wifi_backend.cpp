#include "platform/wifi_backend.h"

#include <cstdlib>
#include <sstream>
#include <string>
#include <unistd.h>

#include "platform/wpa_ctrl_client.h"

namespace network_service {

namespace {

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

bool valid_network_id(int network_id) {
    return network_id >= 0 && network_id <= 4096;
}

bool wpa_quote(const std::string &value, std::string &quoted, std::string &error) {
    quoted.clear();
    quoted.push_back('"');
    for (unsigned char ch : value) {
        if (ch == '\0' || ch == '\n' || ch == '\r') {
            error = "wpa value contains unsupported control characters";
            quoted.clear();
            return false;
        }
        if (ch == '\\' || ch == '"') quoted.push_back('\\');
        quoted.push_back(static_cast<char>(ch));
    }
    quoted.push_back('"');
    return true;
}

bool run_command(const std::string &cmd, std::string &error) {
    const int rc = system(cmd.c_str());
    if (rc != 0) {
        std::ostringstream os;
        os << "command failed rc=" << rc << ": " << cmd;
        error = os.str();
        return false;
    }
    return true;
}

std::string trim(const std::string &value) {
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

std::vector<std::string> split_tab_line(const std::string &line) {
    std::vector<std::string> fields;
    size_t start = 0;
    while (start <= line.size()) {
        const size_t tab = line.find('\t', start);
        if (tab == std::string::npos) {
            fields.push_back(line.substr(start));
            break;
        }
        fields.push_back(line.substr(start, tab - start));
        start = tab + 1;
    }
    return fields;
}

bool wpa_request(const std::string &iface,
                 const std::string &command,
                 std::string &reply,
                 std::string &error) {
    if (!is_safe_iface(iface)) {
        error = "invalid wifi iface";
        return false;
    }
    WpaCtrlClient ctrl(wpa_ctrl_path_for(iface));
    return ctrl.request(command, reply, error);
}

bool wpa_ok(const std::string &iface,
            const std::string &command,
            std::string &error) {
    std::string reply;
    if (!wpa_request(iface, command, reply, error)) return false;
    const std::string normalized = trim(reply);
    if (normalized == "OK") {
        error.clear();
        return true;
    }
    error = "wpa_supplicant returned: " + normalized;
    return false;
}

int parse_network_id(const std::string &value) {
    char *end = nullptr;
    const long id = strtol(value.c_str(), &end, 10);
    if (end == value.c_str() || id < 0 || id > 4096) return -1;
    return static_cast<int>(id);
}

int wpa_add_network(const std::string &iface, std::string &error) {
    std::string reply;
    if (!wpa_request(iface, "ADD_NETWORK", reply, error)) return -1;
    const std::string normalized = trim(reply);
    const int id = parse_network_id(normalized);
    if (id < 0 && error.empty()) error = "invalid ADD_NETWORK result: " + normalized;
    return id;
}

void parse_saved_flags(const std::string &flags, WifiSavedNetwork &record) {
    record.is_current = flags.find("CURRENT") != std::string::npos;
    record.is_disabled = flags.find("DISABLED") != std::string::npos;
    record.is_temp_disabled = flags.find("TEMP-DISABLED") != std::string::npos;
    record.autoconnect = !record.is_disabled;
}

std::vector<WifiSavedNetwork> parse_saved_networks(const std::string &output) {
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
        const std::vector<std::string> fields = split_tab_line(line);
        if (fields.size() < 2) continue;
        WifiSavedNetwork record;
        record.network_id = parse_network_id(fields[0]);
        record.ssid = fields[1];
        record.bssid = fields.size() >= 3 ? fields[2] : "";
        record.flags = fields.size() >= 4 ? fields[3] : "";
        parse_saved_flags(record.flags, record);
        if (record.network_id >= 0) records.push_back(record);
    }
    return records;
}

std::vector<WifiApRecord> parse_scan_results(const std::string &output) {
    std::vector<WifiApRecord> records;
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
        const std::vector<std::string> fields = split_tab_line(line);
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

} // namespace

bool wifi_ensure_interface_up(const std::string &iface, std::string &error) {
    if (!is_safe_iface(iface)) {
        error = "invalid wifi iface";
        return false;
    }
    return run_command("ifconfig " + iface + " up", error);
}

bool wifi_set_enabled(const std::string &iface, bool enabled, std::string &error) {
    if (!is_safe_iface(iface)) {
        error = "invalid wifi iface";
        return false;
    }
    if (enabled) {
        if (!wifi_ensure_interface_up(iface, error)) return false;
        std::string ignored;
        (void)wpa_ok(iface, "RECONNECT", ignored);
        error.clear();
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
    std::string ignored;
    (void)wpa_ok(iface, "DISCONNECT", ignored);
    error.clear();
    return true;
}

int wifi_create_profile(const std::string &iface,
                        const std::string &ssid,
                        const std::string &password,
                        std::string &error) {
    error.clear();
    if (!is_safe_iface(iface)) {
        error = "invalid wifi iface";
        return -1;
    }
    if (ssid.empty()) {
        error = "ssid is required";
        return -1;
    }

    const int id = wpa_add_network(iface, error);
    if (id < 0) return -1;

    std::string quoted_ssid;
    if (!wpa_quote(ssid, quoted_ssid, error)) return -1;
    if (!wpa_ok(iface,
                "SET_NETWORK " + std::to_string(id) + " ssid " + quoted_ssid,
                error)) {
        return -1;
    }

    if (password.empty()) {
        if (!wpa_ok(iface,
                    "SET_NETWORK " + std::to_string(id) + " key_mgmt NONE",
                    error)) {
            return -1;
        }
    } else {
        std::string quoted_psk;
        if (!wpa_quote(password, quoted_psk, error)) return -1;
        if (!wpa_ok(iface,
                    "SET_NETWORK " + std::to_string(id) + " psk " + quoted_psk,
                    error)) {
            return -1;
        }
    }

    error.clear();
    return id;
}

int wifi_find_profile(const std::string &iface,
                      const std::string &ssid,
                      std::string &error) {
    if (!is_safe_iface(iface)) {
        error = "invalid wifi iface";
        return -1;
    }
    if (ssid.empty()) {
        error = "ssid is required";
        return -1;
    }

    std::string reply;
    if (!wpa_request(iface, "LIST_NETWORKS", reply, error)) return -1;
    const std::vector<WifiSavedNetwork> records = parse_saved_networks(reply);
    for (const auto &record : records) {
        if (record.ssid == ssid) {
            error.clear();
            return record.network_id;
        }
    }
    error = "saved network not found: " + ssid;
    return -1;
}

bool wifi_disable_all_profiles(const std::string &iface, std::string &error) {
    return wpa_ok(iface, "DISABLE_NETWORK all", error);
}

bool wifi_set_profile_enabled(const std::string &iface,
                              int network_id,
                              bool enabled,
                              std::string &error) {
    if (!valid_network_id(network_id)) {
        error = "invalid Wi-Fi profile id";
        return false;
    }
    return wpa_ok(iface,
                  std::string(enabled ? "ENABLE_NETWORK " : "DISABLE_NETWORK ") +
                      std::to_string(network_id),
                  error);
}

bool wifi_select_profile(const std::string &iface,
                         int network_id,
                         std::string &error) {
    if (!valid_network_id(network_id)) {
        error = "invalid Wi-Fi profile id";
        return false;
    }
    return wpa_ok(iface, "SELECT_NETWORK " + std::to_string(network_id), error);
}

bool wifi_remove_profile(const std::string &iface,
                         int network_id,
                         std::string &error) {
    if (!valid_network_id(network_id)) {
        error = "invalid Wi-Fi profile id";
        return false;
    }
    return wpa_ok(iface, "REMOVE_NETWORK " + std::to_string(network_id), error);
}

bool wifi_save_profiles(const std::string &iface, std::string &error) {
    return wpa_ok(iface, "SAVE_CONFIG", error);
}

bool wifi_scan_start(const std::string &iface, std::string &error) {
    error.clear();
    if (!wifi_ensure_interface_up(iface, error)) return false;
    return wpa_ok(iface, "SCAN", error);
}

std::vector<WifiApRecord> wifi_scan_results(const std::string &iface,
                                            std::string &error) {
    error.clear();
    if (!is_safe_iface(iface)) {
        error = "invalid wifi iface";
        return {};
    }
    std::string reply;
    if (!wpa_request(iface, "SCAN_RESULTS", reply, error)) return {};
    return parse_scan_results(reply);
}

std::vector<WifiApRecord> wifi_scan(const std::string &iface, std::string &error) {
    std::vector<WifiApRecord> records;
    if (!wifi_scan_start(iface, error)) return records;
    usleep(1200 * 1000);
    return wifi_scan_results(iface, error);
}

std::vector<WifiSavedNetwork> wifi_list_saved(const std::string &iface,
                                              std::string &error) {
    if (!is_safe_iface(iface)) {
        error = "invalid wifi iface";
        return {};
    }
    std::string reply;
    if (!wpa_request(iface, "LIST_NETWORKS", reply, error)) return {};
    return parse_saved_networks(reply);
}

} // namespace network_service
