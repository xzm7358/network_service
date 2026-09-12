#ifndef NETWORK_SERVICE_WIFI_BACKEND_H
#define NETWORK_SERVICE_WIFI_BACKEND_H

#include <string>
#include <vector>

#include "network_service_contracts.h"

namespace network_service {

// Interface/supplicant mechanisms. Product profile-selection and persistence
// policy belongs to Service, not this Platform adapter.
bool wifi_set_enabled(const std::string &iface, bool enabled, std::string &error);
bool wifi_disconnect(const std::string &iface, std::string &error);
bool wifi_ensure_interface_up(const std::string &iface, std::string &error);

int wifi_create_profile(const std::string &iface,
                        const std::string &ssid,
                        const std::string &password,
                        std::string &error);
int wifi_find_profile(const std::string &iface,
                      const std::string &ssid,
                      std::string &error);
bool wifi_disable_all_profiles(const std::string &iface, std::string &error);
bool wifi_set_profile_enabled(const std::string &iface,
                              int network_id,
                              bool enabled,
                              std::string &error);
bool wifi_select_profile(const std::string &iface,
                         int network_id,
                         std::string &error);
bool wifi_remove_profile(const std::string &iface,
                         int network_id,
                         std::string &error);
bool wifi_save_profiles(const std::string &iface, std::string &error);

bool wifi_scan_start(const std::string &iface, std::string &error);
std::vector<WifiApRecord> wifi_scan_results(const std::string &iface, std::string &error);
std::vector<WifiApRecord> wifi_scan(const std::string &iface, std::string &error);
std::vector<WifiSavedNetwork> wifi_list_saved(const std::string &iface, std::string &error);

} // namespace network_service

#endif // NETWORK_SERVICE_WIFI_BACKEND_H
