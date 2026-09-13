#include "service/network_daemon.h"

#include <iostream>
#include <string>
#include <utility>

#ifndef NETWORK_SERVICE_VERSION
#define NETWORK_SERVICE_VERSION "0.1.0"
#endif

#include "config/ethernet_config.h"
#include "platform/dhcp_lease_store.h"
#include "platform/interface_snapshot.h"
#include "platform/netlink_monitor.h"
#include "platform/network_configurator.h"
#include "platform/udhcpc_process.h"
#include "platform/wifi_backend.h"
#include "platform/wpa_event_monitor.h"
#include "service/network_control_plane.h"
#include "service/network_state.h"
#include "service/wifi_manager.h"
#include "service/wifi_profile_policy.h"

namespace network_service {

namespace {

static WifiRuntimeFact make_wifi_runtime_fact(const WpaEventFact &events,
                                              const WifiManagerState &manager) {
    WifiRuntimeFact fact;
    fact.l2_state = events.l2_state;
    fact.dhcp_state = manager.dhcp_state;
    fact.failure_reason = !manager.failure_reason.empty()
                              ? manager.failure_reason
                              : events.failure_reason;
    return fact;
}

static WpaEventsView project_wpa_compatibility(const WpaEventFact &events,
                                               const NetworkSnapshot &truth,
                                               const WifiManagerState &manager) {
    const WifiRuntimeFact runtime = make_wifi_runtime_fact(events, manager);
    WpaEventsView view;
    view.attached = events.attached;
    view.connected = truth.wifi.connected;
    view.disconnected = runtime.l2_state == WifiL2State::Disconnected;
    view.dhcp_requested = dhcp_client_active(manager.dhcp_state);
    view.has_ip = truth.wifi.has_ip;
    view.has_default_route = truth.wifi.has_default_route;
    view.dns_available = truth.dns_available;
    view.ip4 = truth.wifi.ip4;
    view.gateway4 = truth.wifi.gateway4;
    view.dns4 = truth.dns4;
    view.connect_events = events.connect_events;
    view.disconnect_events = events.disconnect_events;
    view.dhcp_requests = manager.dhcp_requests;
    view.scan_started_events = events.scan_started_events;
    view.scan_result_events = events.scan_result_events;
    view.scan_failed_events = events.scan_failed_events;
    view.event_sequence = events.event_sequence;
    view.last_scan_started_sequence = events.last_scan_started_sequence;
    view.last_scan_result_sequence = events.last_scan_result_sequence;
    view.last_scan_failed_sequence = events.last_scan_failed_sequence;
    view.wifi_state = legacy_wifi_state(truth, runtime);
    view.failure_reason = runtime.failure_reason;
    view.last_event = events.last_event;
    view.last_ssid = events.last_ssid;
    view.last_bssid = events.last_bssid;
    return view;
}

static WifiCommandResult command_result(std::string requested) {
    WifiCommandResult result;
    result.requested = std::move(requested);
    return result;
}

static void adopt_existing_dhcp(const std::string &iface,
                                NetworkControlPlane &control_plane,
                                WifiManager *wifi_manager) {
    UdhcpcProbeResult probe;
    std::string error;
    if (!UdhcpcProcess::probe(iface, probe, error)) {
        std::cerr << "network_service: DHCP_BROWNFIELD_PROBE_FAILED iface="
                  << iface << " error=" << error << std::endl;
        return;
    }

    switch (probe.state) {
    case UdhcpcOwnershipState::OwnedRunning:
        if (!control_plane.adopt_dhcp(iface, error)) {
            std::cerr << "network_service: DHCP_BROWNFIELD_ADOPT_FAILED iface="
                      << iface << " error=" << error << std::endl;
            return;
        }
        if (wifi_manager) wifi_manager->adopt_dhcp_running();
        std::cerr << "network_service: DHCP_BROWNFIELD_ADOPTED iface=" << iface
                  << " pid=" << probe.pid
                  << " generation=" << probe.generation
                  << " lease=" << (probe.lease_exists ? "present" : "pending")
                  << std::endl;
        return;

    case UdhcpcOwnershipState::StaleArtifacts:
        UdhcpcProcess::cleanup_stale(iface);
        std::cerr << "network_service: DHCP_BROWNFIELD_STALE_CLEANUP iface="
                  << iface << std::endl;
        return;

    case UdhcpcOwnershipState::ConflictingProcess:
        std::cerr << "network_service: DHCP_BROWNFIELD_CONFLICT iface=" << iface
                  << " pid=" << probe.pid << std::endl;
        return;

    case UdhcpcOwnershipState::Absent:
    default:
        return;
    }
}

static bool apply_ethernet_runtime(NetworkControlPlane &control_plane,
                                   const std::string &iface,
                                   const EthernetConfig &config,
                                   std::string &error) {
    error.clear();
    if (config.method == "static") {
        return control_plane.apply_ethernet_static(config.ip4,
                                                   config.netmask4,
                                                   config.gateway4,
                                                   config.dns_enabled ? config.dns4 : "",
                                                   config.route_metric,
                                                   error);
    }
    return control_plane.start_dhcp(iface, error);
}

static std::string failure_with_rollback(const std::string &failure,
                                         const std::string &rollback_error) {
    if (rollback_error.empty()) return failure;
    return failure + "; rollback_failed: " + rollback_error;
}

static NetworkOperationResult<EthernetConfig> apply_ethernet_transaction(
    NetworkControlPlane *control_plane,
    const std::string &config_dir,
    const std::string &iface,
    EthernetConfig desired) {
    if (!control_plane) {
        return NetworkOperationResult<EthernetConfig>::failure(
            500, "network control plane unavailable");
    }

    const EthernetConfig previous = load_ethernet_config(config_dir, iface);

    EthernetConfigStage stage;
    std::string error;
    if (!stage_ethernet_config(config_dir, desired, stage, error)) {
        return NetworkOperationResult<EthernetConfig>::failure(
            500, "failed to stage ethernet config: " + error);
    }

    if (!apply_ethernet_runtime(*control_plane, iface, desired, error)) {
        const std::string failure = error.empty()
                                        ? "failed to apply ethernet runtime config"
                                        : error;
        discard_ethernet_config(stage);

        std::string rollback_error;
        (void)apply_ethernet_runtime(*control_plane, iface, previous, rollback_error);
        return NetworkOperationResult<EthernetConfig>::failure(
            500, failure_with_rollback(failure, rollback_error));
    }

    const EthernetConfigCommitResult commit_result =
        commit_ethernet_config(stage, error);
    if (commit_result == EthernetConfigCommitResult::Failed) {
        const std::string failure = error.empty()
                                        ? "failed to commit ethernet config"
                                        : error;
        discard_ethernet_config(stage);

        std::string rollback_error;
        (void)apply_ethernet_runtime(*control_plane, iface, previous, rollback_error);
        return NetworkOperationResult<EthernetConfig>::failure(
            500, failure_with_rollback(failure, rollback_error));
    }

    if (commit_result == EthernetConfigCommitResult::CommittedDurabilityUncertain) {
        // rename() already made desired config the logical durable truth. Keep
        // runtime aligned with that committed file; rolling back runtime here
        // would create the exact split-brain this transaction is intended to
        // prevent. Surface the durability concern operationally without turning
        // a logically committed operation into an IPC failure.
        std::cerr << "network_service: ETHERNET_CONFIG_DURABILITY_UNCERTAIN error="
                  << error << std::endl;
    }

    return NetworkOperationResult<EthernetConfig>::success(std::move(desired));
}

} // namespace

NetworkDaemon::NetworkDaemon(std::string eth_iface,
                             std::string wifi_iface,
                             std::string config_dir,
                             std::string event_dir,
                             SnapshotProvider snapshot_provider)
    : eth_iface_(std::move(eth_iface)),
      wifi_iface_(std::move(wifi_iface)),
      config_dir_(std::move(config_dir)),
      snapshot_provider_(std::move(snapshot_provider)) {
    NetworkControlPlaneOps ops;
    ops.start_dhcp = [](const std::string &iface, std::string &error) {
        return UdhcpcProcess::start(iface, error);
    };
    ops.stop_dhcp = [](const std::string &iface) {
        UdhcpcProcess::stop(iface);
    };
    ops.read_lease = [](const std::string &iface,
                        DhcpLeaseFact &fact,
                        bool &exists,
                        std::string &error) {
        return DhcpLeaseStore::read(iface, fact, exists, error);
    };
    ops.clear_lease = [](const std::string &iface) {
        DhcpLeaseStore::clear(iface);
    };
    ops.apply_ipv4 = [](const std::string &iface,
                        const std::string &ip4,
                        const std::string &netmask4,
                        std::string &error) {
        return NetworkConfigurator::apply_ipv4(iface, ip4, netmask4, error);
    };
    ops.clear_ipv4 = [](const std::string &iface, std::string &error) {
        return NetworkConfigurator::clear_ipv4(iface, error);
    };
    ops.set_default_route = [](const std::string &iface,
                               const std::string &gateway4,
                               int metric,
                               std::string &error) {
        return NetworkConfigurator::set_default_route(iface, gateway4, metric, error);
    };
    ops.clear_default_route = [](const std::string &iface,
                                 const std::string &gateway4,
                                 int metric,
                                 std::string &error) {
        return NetworkConfigurator::clear_default_route(iface, gateway4, metric, error);
    };
    ops.set_dns = [](const std::string &dns4, std::string &error) {
        return NetworkConfigurator::set_primary_dns(dns4, error);
    };
    ops.clear_dns = [](std::string &error) {
        return NetworkConfigurator::clear_dns(error);
    };
    ops.snapshot = [this]() {
        return read_live_snapshot(eth_iface_.c_str(), wifi_iface_.c_str());
    };

    control_plane_.reset(new NetworkControlPlane(eth_iface_, wifi_iface_, std::move(ops)));
    wifi_manager_.reset(new WifiManager(
        [this](std::string &error) {
            if (!control_plane_) {
                error = "network control plane unavailable";
                return false;
            }
            return control_plane_->start_dhcp(wifi_iface_, error);
        },
        [this]() {
            if (control_plane_) control_plane_->stop_dhcp(wifi_iface_);
        },
        [this]() {
            return UdhcpcProcess::is_running(wifi_iface_);
        }));

    // Deterministic injected-snapshot fixtures must remain isolated from host
    // process artifacts. Production construction adopts verified owned DHCP
    // processes before WPA monitoring can emit a CONNECTED event and start a
    // duplicate lifecycle.
    if (!snapshot_provider_) {
        adopt_existing_dhcp(eth_iface_, *control_plane_, nullptr);
        adopt_existing_dhcp(wifi_iface_, *control_plane_, wifi_manager_.get());
    }

    WifiProfilePolicyOps profile_ops;
    profile_ops.ensure_interface_up = [this](std::string &error) {
        return network_service::wifi_ensure_interface_up(wifi_iface_, error);
    };
    profile_ops.create_profile = [this](const std::string &ssid,
                                        const std::string &password,
                                        std::string &error) {
        return network_service::wifi_create_profile(wifi_iface_, ssid, password, error);
    };
    profile_ops.find_profile = [this](const std::string &ssid, std::string &error) {
        return network_service::wifi_find_profile(wifi_iface_, ssid, error);
    };
    profile_ops.disable_all_profiles = [this](std::string &error) {
        return network_service::wifi_disable_all_profiles(wifi_iface_, error);
    };
    profile_ops.set_profile_enabled = [this](int network_id,
                                             bool enabled,
                                             std::string &error) {
        return network_service::wifi_set_profile_enabled(
            wifi_iface_, network_id, enabled, error);
    };
    profile_ops.select_profile = [this](int network_id, std::string &error) {
        return network_service::wifi_select_profile(wifi_iface_, network_id, error);
    };
    profile_ops.remove_profile = [this](int network_id, std::string &error) {
        return network_service::wifi_remove_profile(wifi_iface_, network_id, error);
    };
    profile_ops.save_profiles = [this](std::string &error) {
        return network_service::wifi_save_profiles(wifi_iface_, error);
    };
    wifi_profile_policy_.reset(new WifiProfilePolicy(std::move(profile_ops)));

    wpa_monitor_.reset(new WpaEventMonitor(
        wifi_iface_,
        std::move(event_dir),
        [this](bool connected) {
            if (!wifi_manager_) return;
            if (connected) {
                wifi_manager_->on_l2_connected();
            } else {
                wifi_manager_->on_l2_disconnected();
            }
        }));
    wpa_monitor_->start();

    wifi_scan_lifecycle_.reset(new WifiScanLifecycle(
        [this](std::string &error) {
            return network_service::wifi_scan_start(wifi_iface_, error);
        },
        [this](std::string &error) {
            return network_service::wifi_scan_results(wifi_iface_, error);
        },
        [this]() {
            const WpaEventFact events = wpa_monitor_->snapshot();
            WifiScanEventMarkers markers;
            markers.sequence = events.event_sequence;
            markers.started_sequence = events.last_scan_started_sequence;
            markers.completed_sequence = events.last_scan_result_sequence;
            markers.failed_sequence = events.last_scan_failed_sequence;
            return markers;
        }));

    if (!snapshot_provider_) {
        netlink_monitor_.reset(new NetlinkMonitor());
        std::string netlink_error;
        if (!netlink_monitor_->open(netlink_error)) {
            std::cerr << "network_service: NETLINK_MONITOR_UNAVAILABLE error="
                      << netlink_error << std::endl;
            netlink_monitor_.reset();
        }
    }
}

NetworkDaemon::~NetworkDaemon() = default;

bool NetworkDaemon::reconcile(std::string &error) {
    if (!control_plane_) {
        error = "network control plane unavailable";
        return false;
    }
    if (wifi_manager_) (void)wifi_manager_->reconcile_dhcp_process();
    return control_plane_->reconcile(error);
}

bool NetworkDaemon::refresh_external_state(std::string &error) {
    if (!control_plane_) {
        error = "network control plane unavailable";
        return false;
    }
    return control_plane_->refresh_external_state(error);
}

int NetworkDaemon::network_event_fd() const {
    return netlink_monitor_ ? netlink_monitor_->fd() : -1;
}

bool NetworkDaemon::consume_network_events(bool &changed, std::string &error) {
    changed = false;
    error.clear();
    if (!netlink_monitor_) return true;
    if (!netlink_monitor_->drain(changed, error)) return false;
    if (!changed) return true;
    return refresh_external_state(error);
}

NetworkSnapshot NetworkDaemon::snapshot() const {
    if (snapshot_provider_) return snapshot_provider_();

    NetworkSnapshot truth = read_live_snapshot(eth_iface_.c_str(), wifi_iface_.c_str());
    WpaEventFact events;
    WifiManagerState manager;
    if (wpa_monitor_) events = wpa_monitor_->snapshot();
    if (wifi_manager_) manager = wifi_manager_->state();

    normalize_network_snapshot(truth, make_wifi_runtime_fact(events, manager));
    if (control_plane_) truth.route_policy = control_plane_->route_policy();
    return truth;
}

PingInfo NetworkDaemon::ping() const {
    PingInfo info;
    info.service = "network_service";
    info.version = NETWORK_SERVICE_VERSION;
    info.mode = "explicit_apply";
    return info;
}

NetworkOperationResult<WpaEventsView> NetworkDaemon::wpa_events() const {
    WpaEventFact events;
    WifiManagerState manager;
    if (wpa_monitor_) events = wpa_monitor_->snapshot();
    if (wifi_manager_) manager = wifi_manager_->state();
    return NetworkOperationResult<WpaEventsView>::success(
        project_wpa_compatibility(events, snapshot(), manager));
}

NetworkOperationResult<EthernetConfig> NetworkDaemon::eth_get_config() const {
    return NetworkOperationResult<EthernetConfig>::success(
        load_ethernet_config(config_dir_, eth_iface_));
}

NetworkOperationResult<EthernetConfig> NetworkDaemon::eth_set_dhcp() const {
    EthernetConfig config = load_ethernet_config(config_dir_, eth_iface_);
    config.iface = eth_iface_;
    config.method = "dhcp";
    config.ip4.clear();
    config.netmask4.clear();
    config.gateway4.clear();
    config.dns4.clear();
    config.route_metric = 10;
    config.dns_enabled = true;

    return apply_ethernet_transaction(control_plane_.get(),
                                      config_dir_,
                                      eth_iface_,
                                      std::move(config));
}

NetworkOperationResult<EthernetConfig> NetworkDaemon::eth_set_static(
    const std::string &ip,
    const std::string &mask,
    const std::string &gateway,
    const std::string &dns) const {
    EthernetConfig config = load_ethernet_config(config_dir_, eth_iface_);
    config.iface = eth_iface_;
    config.method = "static";
    config.ip4 = ip;
    config.netmask4 = mask;
    config.gateway4 = gateway;
    config.dns4 = dns;
    config.route_metric = 10;
    config.dns_enabled = !dns.empty();

    return apply_ethernet_transaction(control_plane_.get(),
                                      config_dir_,
                                      eth_iface_,
                                      std::move(config));
}

NetworkOperationResult<std::vector<WifiApRecord>> NetworkDaemon::wifi_scan() const {
    std::string error;
    std::vector<WifiApRecord> records = network_service::wifi_scan(wifi_iface_, error);
    if (!error.empty() && records.empty()) {
        return NetworkOperationResult<std::vector<WifiApRecord>>::failure(
            500, std::move(error));
    }
    return NetworkOperationResult<std::vector<WifiApRecord>>::success(std::move(records));
}

NetworkOperationResult<WifiScanStatus> NetworkDaemon::wifi_scan_start() {
    if (!wifi_scan_lifecycle_) {
        return NetworkOperationResult<WifiScanStatus>::failure(
            500, "wifi scan lifecycle unavailable");
    }
    WifiScanStatus status = wifi_scan_lifecycle_->start();
    if (status.state == WifiScanState::Failed) {
        return NetworkOperationResult<WifiScanStatus>::failure(500, status.error);
    }
    return NetworkOperationResult<WifiScanStatus>::success(std::move(status), 202);
}

NetworkOperationResult<WifiScanStatus> NetworkDaemon::wifi_scan_status() {
    if (!wifi_scan_lifecycle_) {
        return NetworkOperationResult<WifiScanStatus>::failure(
            500, "wifi scan lifecycle unavailable");
    }
    return NetworkOperationResult<WifiScanStatus>::success(wifi_scan_lifecycle_->poll());
}

NetworkOperationResult<WifiEnabledResult> NetworkDaemon::wifi_set_enabled(bool enabled) const {
    if (!enabled && wifi_manager_) wifi_manager_->stop_dhcp();

    std::string error;
    if (!network_service::wifi_set_enabled(wifi_iface_, enabled, error)) {
        return NetworkOperationResult<WifiEnabledResult>::failure(500, std::move(error));
    }
    WifiEnabledResult result;
    result.enabled = enabled;
    return NetworkOperationResult<WifiEnabledResult>::success(result);
}

NetworkOperationResult<WifiCommandResult> NetworkDaemon::wifi_connect(
    const std::string &ssid,
    const std::string &password) const {
    if (!wifi_profile_policy_) {
        return NetworkOperationResult<WifiCommandResult>::failure(
            500, "Wi-Fi profile policy unavailable");
    }
    std::string error;
    if (!wifi_profile_policy_->connect_new(ssid, password, error)) {
        return NetworkOperationResult<WifiCommandResult>::failure(500, std::move(error));
    }
    return NetworkOperationResult<WifiCommandResult>::success(command_result("connect"));
}

NetworkOperationResult<WifiCommandResult> NetworkDaemon::wifi_connect_saved(
    const std::string &ssid) const {
    if (!wifi_profile_policy_) {
        return NetworkOperationResult<WifiCommandResult>::failure(
            500, "Wi-Fi profile policy unavailable");
    }
    std::string error;
    if (!wifi_profile_policy_->connect_saved(ssid, error)) {
        return NetworkOperationResult<WifiCommandResult>::failure(500, std::move(error));
    }
    return NetworkOperationResult<WifiCommandResult>::success(command_result("connect_saved"));
}

NetworkOperationResult<std::vector<WifiSavedNetwork>> NetworkDaemon::wifi_list_saved() const {
    std::string error;
    std::vector<WifiSavedNetwork> records =
        network_service::wifi_list_saved(wifi_iface_, error);
    if (!error.empty() && records.empty()) {
        return NetworkOperationResult<std::vector<WifiSavedNetwork>>::failure(
            500, std::move(error));
    }
    return NetworkOperationResult<std::vector<WifiSavedNetwork>>::success(std::move(records));
}

NetworkOperationResult<WifiCommandResult> NetworkDaemon::wifi_forget(
    const std::string &ssid) const {
    if (!wifi_profile_policy_) {
        return NetworkOperationResult<WifiCommandResult>::failure(
            500, "Wi-Fi profile policy unavailable");
    }
    std::string error;
    if (!wifi_profile_policy_->forget(ssid, error)) {
        return NetworkOperationResult<WifiCommandResult>::failure(500, std::move(error));
    }
    return NetworkOperationResult<WifiCommandResult>::success(command_result("forget"));
}

NetworkOperationResult<WifiCommandResult> NetworkDaemon::wifi_set_autoconnect(
    const std::string &ssid,
    bool enabled) const {
    if (!wifi_profile_policy_) {
        return NetworkOperationResult<WifiCommandResult>::failure(
            500, "Wi-Fi profile policy unavailable");
    }
    std::string error;
    if (!wifi_profile_policy_->set_autoconnect(ssid, enabled, error)) {
        return NetworkOperationResult<WifiCommandResult>::failure(500, std::move(error));
    }
    WifiCommandResult result = command_result("autoconnect");
    result.has_enabled = true;
    result.enabled = enabled;
    return NetworkOperationResult<WifiCommandResult>::success(std::move(result));
}

NetworkOperationResult<WifiCommandResult> NetworkDaemon::wifi_disconnect() const {
    if (wifi_manager_) wifi_manager_->stop_dhcp();

    std::string error;
    if (!network_service::wifi_disconnect(wifi_iface_, error)) {
        return NetworkOperationResult<WifiCommandResult>::failure(500, std::move(error));
    }
    return NetworkOperationResult<WifiCommandResult>::success(command_result("disconnect"));
}

} // namespace network_service
