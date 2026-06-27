#ifndef NETWORK_SERVICE_WIFI_BACKEND_H
#define NETWORK_SERVICE_WIFI_BACKEND_H

#include <string>
#include <vector>

namespace network_service {

struct WifiApRecord {
    std::string bssid;
    int frequency = 0;
    int signal_dbm = 0;
    std::string flags;
    std::string ssid;
};

struct WifiSavedNetwork {
    int network_id = -1;
    std::string ssid;
    std::string bssid;
    std::string flags;
    bool is_current = false;
    bool is_disabled = false;
    bool is_temp_disabled = false;
    bool autoconnect = true;
};

bool wifi_set_enabled(const std::string &iface, bool enabled, std::string &error);
bool wifi_disconnect(const std::string &iface, std::string &error);
bool wifi_start_dhcp(const std::string &iface, std::string &error);
bool wifi_connect(const std::string &iface,
                  const std::string &ssid,
                  const std::string &password,
                  std::string &error);
bool wifi_connect_saved(const std::string &iface, const std::string &ssid, std::string &error);
bool wifi_forget_saved(const std::string &iface, const std::string &ssid, std::string &error);
bool wifi_set_autoconnect(const std::string &iface,
                          const std::string &ssid,
                          bool enabled,
                          std::string &error);
std::vector<WifiApRecord> wifi_scan(const std::string &iface, std::string &error);
std::vector<WifiSavedNetwork> wifi_list_saved(const std::string &iface, std::string &error);
std::string wifi_scan_to_json(const std::vector<WifiApRecord> &records);
std::string wifi_saved_to_json(const std::vector<WifiSavedNetwork> &records);

} // namespace network_service

#endif // NETWORK_SERVICE_WIFI_BACKEND_H
