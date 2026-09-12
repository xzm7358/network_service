#ifndef NETWORK_SERVICE_DAEMON_H
#define NETWORK_SERVICE_DAEMON_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "network_service_types.h"

namespace network_service {

class NetlinkMonitor;
class NetworkControlPlane;
class WifiManager;
class WpaEventMonitor;
class WifiScanLifecycle;

class NetworkDaemon {
public:
    using SnapshotProvider = std::function<NetworkSnapshot()>;

    NetworkDaemon(std::string eth_iface,
                  std::string wifi_iface,
                  std::string config_dir,
                  std::string event_dir,
                  SnapshotProvider snapshot_provider = {});
    ~NetworkDaemon();

    // Fast DHCP lease-fact reconciliation. Does not perform periodic full-state
    // observation when lease facts are unchanged.
    bool reconcile(std::string &error);
    bool reconcile(bool &changed, std::string &error);

    // Refresh policy that depends on externally-managed route/DNS state.
    bool refresh_external_state(std::string &error);

    // Service facade for reactor-owned event handling. IPC remains independent
    // of Netlink/WPA implementation types.
    int network_event_fd() const;
    bool consume_network_events(bool &changed, std::string &error);
    bool consume_runtime_state_dirty();

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
    std::string wifi_scan_start_json();
    std::string wifi_scan_status_json();
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
    std::unique_ptr<NetworkControlPlane> control_plane_;
    std::unique_ptr<WifiManager> wifi_manager_;
    std::unique_ptr<WpaEventMonitor> wpa_monitor_;
    std::unique_ptr<WifiScanLifecycle> wifi_scan_lifecycle_;
    std::unique_ptr<NetlinkMonitor> netlink_monitor_;
    std::atomic<std::uint64_t> last_wpa_event_sequence_{0};
};

} // namespace network_service

#endif // NETWORK_SERVICE_DAEMON_H
