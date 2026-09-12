#ifndef NETWORK_SERVICE_DAEMON_H
#define NETWORK_SERVICE_DAEMON_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "config/ethernet_config.h"
#include "network_service_types.h"
#include "platform/wifi_backend.h"
#include "service/network_operation_result.h"
#include "service/wifi_scan_lifecycle.h"
#include "service/wpa_events_view.h"

namespace network_service {

class NetlinkMonitor;
class NetworkControlPlane;
class WifiManager;
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

    bool reconcile(std::string &error);
    bool reconcile(bool &changed, std::string &error);
    bool refresh_external_state(std::string &error);

    int network_event_fd() const;
    bool consume_network_events(bool &changed, std::string &error);
    bool consume_runtime_state_dirty();

    NetworkSnapshot snapshot() const;
    PingInfo ping() const;
    NetworkOperationResult<WpaEventsView> wpa_events() const;
    NetworkOperationResult<EthernetConfig> eth_get_config() const;
    NetworkOperationResult<EthernetConfig> eth_set_dhcp() const;
    NetworkOperationResult<EthernetConfig> eth_set_static(const std::string &ip,
                                                          const std::string &mask,
                                                          const std::string &gateway,
                                                          const std::string &dns) const;
    NetworkOperationResult<std::vector<WifiApRecord>> wifi_scan() const;
    NetworkOperationResult<WifiScanStatus> wifi_scan_start();
    NetworkOperationResult<WifiScanStatus> wifi_scan_status();
    NetworkOperationResult<WifiEnabledResult> wifi_set_enabled(bool enabled) const;
    NetworkOperationResult<WifiCommandResult> wifi_connect(const std::string &ssid,
                                                           const std::string &password) const;
    NetworkOperationResult<WifiCommandResult> wifi_connect_saved(const std::string &ssid) const;
    NetworkOperationResult<std::vector<WifiSavedNetwork>> wifi_list_saved() const;
    NetworkOperationResult<WifiCommandResult> wifi_forget(const std::string &ssid) const;
    NetworkOperationResult<WifiCommandResult> wifi_set_autoconnect(const std::string &ssid,
                                                                   bool enabled) const;
    NetworkOperationResult<WifiCommandResult> wifi_disconnect() const;

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
