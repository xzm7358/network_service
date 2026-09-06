#ifndef NETWORK_SERVICE_DAEMON_H
#define NETWORK_SERVICE_DAEMON_H

#include <functional>
#include <memory>
#include <string>

#include "network_service_types.h"

namespace network_service {

class WpaEventMonitor;

class NetworkDaemon {
public:
    using SnapshotProvider = std::function<NetworkSnapshot()>;

    NetworkDaemon(std::string eth_iface,
                  std::string wifi_iface,
                  std::string config_dir,
                  std::string event_dir,
                  SnapshotProvider snapshot_provider = {});
    ~NetworkDaemon();

    NetworkSnapshot snapshot() const;
    std::string snapshot_result_json() const;
    std::string snapshot_json() const;
    std::string ping_json() const;
    std::string wpa_events_json() const;
    std::string eth_get_config_json() const;
    std::string eth_set_dhcp_json() const;
    std::string eth_set_static_json(const std::string &ip,
                                    const std::string &mask,
                                    const std::string &gateway,
                                    const std::string &dns) const;
    std::string wifi_scan_json() const;
    std::string wifi_set_enabled_json(bool enabled) const;
    std::string wifi_connect_json(const std::string &ssid, const std::string &password) const;
    std::string wifi_connect_saved_json(const std::string &ssid) const;
    std::string wifi_list_saved_json() const;
    std::string wifi_forget_json(const std::string &ssid) const;
    std::string wifi_set_autoconnect_json(const std::string &ssid, bool enabled) const;
    std::string wifi_disconnect_json() const;

private:
    std::string eth_iface_;
    std::string wifi_iface_;
    std::string config_dir_;
    SnapshotProvider snapshot_provider_;
    std::unique_ptr<WpaEventMonitor> wpa_monitor_;
};

} // namespace network_service

#endif // NETWORK_SERVICE_DAEMON_H
