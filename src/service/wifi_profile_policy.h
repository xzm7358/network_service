#ifndef NETWORK_SERVICE_WIFI_PROFILE_POLICY_H
#define NETWORK_SERVICE_WIFI_PROFILE_POLICY_H

#include <functional>
#include <string>

namespace network_service {

struct WifiProfilePolicyOps {
    std::function<bool(std::string &)> ensure_interface_up;
    std::function<int(const std::string &, const std::string &, std::string &)> create_profile;
    std::function<int(const std::string &, std::string &)> find_profile;
    std::function<bool(std::string &)> disable_all_profiles;
    std::function<bool(int, bool, std::string &)> set_profile_enabled;
    std::function<bool(int, std::string &)> select_profile;
    std::function<bool(int, std::string &)> remove_profile;
    std::function<bool(std::string &)> save_profiles;
};

class WifiProfilePolicy {
public:
    explicit WifiProfilePolicy(WifiProfilePolicyOps ops);

    bool connect_new(const std::string &ssid,
                     const std::string &password,
                     std::string &error) const;
    bool connect_saved(const std::string &ssid, std::string &error) const;
    bool forget(const std::string &ssid, std::string &error) const;
    bool set_autoconnect(const std::string &ssid,
                         bool enabled,
                         std::string &error) const;

private:
    bool activate_profile(int network_id,
                          bool persist,
                          std::string &error) const;

    WifiProfilePolicyOps ops_;
};

} // namespace network_service

#endif // NETWORK_SERVICE_WIFI_PROFILE_POLICY_H
