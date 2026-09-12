#include "service/network_daemon.h"

#include <sstream>
#include <string>
#include <utility>

#ifndef NETWORK_SERVICE_VERSION
#define NETWORK_SERVICE_VERSION "0.1.0"
#endif

#include "config/ethernet_config.h"
#include "platform/dhcp_lease_store.h"
#include "platform/interface_snapshot.h"
#include "platform/network_configurator.h"
#include "platform/udhcpc_process.h"
#include "platform/wifi_backend.h"
#include "platform/wpa_event_monitor.h"
#include "service/network_control_plane.h"
#include "service/wifi_manager.h"
#include "service/wifi_scan_lifecycle.h"

namespace network_service {

namespace {

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

static std::string ok_json(const std::string &result_json) {
    return "{\"status\":200,\"result\":" + result_json + "}";
}

static std::string error_json(int status, const std::string &message) {
    std::ostringstream os;
    os << "{\"status\":" << status << ",\"error\":\"" << json_escape(message) << "\"}";
    return os.str();
}

static void overlay_wpa_with_runtime_state(WpaEventSnapshot &events,
                                           const NetworkSnapshot &live,
                                           const WifiManagerState &manager) {
    events.dhcp_requested = manager.dhcp_requested;
    events.dhcp_requests = manager.dhcp_requests;
    if (!manager.failure_reason.empty()) {
        events.wifi_state = "failed";
        events.failure_reason = manager.failure_reason;
    }

    events.has_ip = live.wifi.has_ip;
    events.has_default_route = live.wifi.has_default_route;
    events.dns_available = live.dns_available;
    events.ip4 = live.wifi.ip4;
    events.gateway4 = live.wifi.gateway4;
    events.dns4 = live.dns4;

    if (events.connected && live.wifi.has_ip && live.wifi.has_default_route && live.dns_available) {
        events.wifi_state = "connected";
        events.disconnected = false;
        events.dhcp_requested = true;
        events.failure_reason.clear();
        return;
    }

    if (events.connected && events.wifi_state == "connected") {
        events.wifi_state = "ip_configuring";
    }
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
    ops.clear_default_route = [](const std::string &iface) {
        NetworkConfigurator::clear_default_route(iface);
    };
    ops.set_dns = [](const std::string &dns4, std::string &error) {
        return NetworkConfigurator::set_primary_dns(dns4, error);
    };
    ops.clear_dns = [](std::string &error) {
        return NetworkConfigurator::clear_dns(error);
    };
    ops.snapshot = [this]() {
        return snapshot();
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
        }));
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
            return wifi_scan_start(wifi_iface_, error);
        },
        [this](std::string &error) {
            return wifi_scan_results(wifi_iface_, error);
        },
        [this]() {
            const WpaEventSnapshot events = wpa_monitor_->snapshot();
            WifiScanEventMarkers markers;
            markers.sequence = events.event_sequence;
            markers.started_sequence = events.last_scan_started_sequence;
            markers.completed_sequence = events.last_scan_result_sequence;
            markers.failed_sequence = events.last_scan_failed_sequence;
            return markers;
        }));
}

NetworkDaemon::~NetworkDaemon() = default;

bool NetworkDaemon::reconcile(std::string &error) {
    if (!control_plane_) {
        error = "network control plane unavailable";
        return false;
    }
    return control_plane_->reconcile(error);
}

NetworkSnapshot NetworkDaemon::snapshot() const {
    if (snapshot_provider_) return snapshot_provider_();
    return read_live_snapshot(eth_iface_.c_str(), wifi_iface_.c_str());
}

std::string NetworkDaemon::snapshot_result_json() const {
    return snapshot_to_json(snapshot());
}

std::string NetworkDaemon::snapshot_json() const {
    return ok_json(snapshot_result_json());
}

std::string NetworkDaemon::ping_json() const {
    std::ostringstream os;
    os << "{\"service\":\"network_service\","
       << "\"version\":\"" << NETWORK_SERVICE_VERSION << "\","
       << "\"mode\":\"explicit_apply\"}";
    return ok_json(os.str());
}

std::string NetworkDaemon::wpa_events_json() const {
    if (!wpa_monitor_) {
        return ok_json("{\"attached\":false}");
    }

    WpaEventSnapshot events = wpa_monitor_->snapshot();
    WifiManagerState manager;
    if (wifi_manager_) manager = wifi_manager_->state();
    overlay_wpa_with_runtime_state(events, snapshot(), manager);
    return ok_json(wpa_event_snapshot_to_json(events));
}

std::string NetworkDaemon::eth_get_config_json() const {
    EthernetConfig config = load_ethernet_config(config_dir_, eth_iface_);
    return ok_json(ethernet_config_to_json(config));
}

std::string NetworkDaemon::eth_set_dhcp_json() const {
    EthernetConfig config = load_ethernet_config(config_dir_, eth_iface_);
    config.iface = eth_iface_;
    config.method = "dhcp";
    config.ip4.clear();
    config.netmask4.clear();
    config.gateway4.clear();
    config.dns4.clear();
    config.route_metric = 10;
    config.dns_enabled = true;

    if (!save_ethernet_config(config_dir_, config)) {
        return error_json(500, "failed to save ethernet config");
    }

    if (!control_plane_) return error_json(500, "network control plane unavailable");
    std::string error;
    if (!control_plane_->start_dhcp(eth_iface_, error)) {
        return error_json(500, error);
    }
    return ok_json(ethernet_config_to_json(config));
}

std::string NetworkDaemon::eth_set_static_json(const std::string &ip,
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

    if (!save_ethernet_config(config_dir_, config)) {
        return error_json(500, "failed to save ethernet config");
    }

    if (!control_plane_) return error_json(500, "network control plane unavailable");
    std::string error;
    if (!control_plane_->apply_ethernet_static(config.ip4,
                                               config.netmask4,
                                               config.gateway4,
                                               config.dns4,
                                               config.route_metric,
                                               error)) {
        return error_json(500, error);
    }
    return ok_json(ethernet_config_to_json(config));
}

std::string NetworkDaemon::wifi_scan_json() const {
    std::string error;
    std::vector<WifiApRecord> records = wifi_scan(wifi_iface_, error);
    if (!error.empty() && records.empty()) {
        return error_json(500, error);
    }
    return ok_json(wifi_scan_to_json(records));
}

static std::string wifi_scan_lifecycle_json(const WifiScanStatus &status) {
    std::ostringstream os;
    os << "{\"scanId\":" << status.scan_id
       << ",\"state\":\"" << wifi_scan_state_name(status.state) << "\""
       << ",\"error\":\"" << json_escape(status.error) << "\""
       << ",\"results\":" << wifi_scan_to_json(status.records)
       << "}";
    return os.str();
}

std::string NetworkDaemon::wifi_scan_start_json() {
    if (!wifi_scan_lifecycle_) {
        return error_json(500, "wifi scan lifecycle unavailable");
    }
    WifiScanStatus status = wifi_scan_lifecycle_->start();
    if (status.state == WifiScanState::Failed) {
        return error_json(500, status.error);
    }
    return std::string("{\"status\":202,\"result\":") +
           wifi_scan_lifecycle_json(status) + "}";
}

std::string NetworkDaemon::wifi_scan_status_json() {
    if (!wifi_scan_lifecycle_) {
        return error_json(500, "wifi scan lifecycle unavailable");
    }
    return ok_json(wifi_scan_lifecycle_json(wifi_scan_lifecycle_->poll()));
}

std::string NetworkDaemon::wifi_set_enabled_json(bool enabled) const {
    if (!enabled && wifi_manager_) {
        wifi_manager_->stop_dhcp();
    }
    std::string error;
    if (!wifi_set_enabled(wifi_iface_, enabled, error)) {
        return error_json(500, error);
    }
    return ok_json(std::string("{\"enabled\":") + (enabled ? "true" : "false") + "}");
}

std::string NetworkDaemon::wifi_connect_json(const std::string &ssid, const std::string &password) const {
    std::string error;
    if (!wifi_connect(wifi_iface_, ssid, password, error)) {
        return error_json(500, error);
    }
    return ok_json("{\"requested\":\"connect\"}");
}

std::string NetworkDaemon::wifi_connect_saved_json(const std::string &ssid) const {
    std::string error;
    if (!wifi_connect_saved(wifi_iface_, ssid, error)) {
        return error_json(500, error);
    }
    return ok_json("{\"requested\":\"connect_saved\"}");
}

std::string NetworkDaemon::wifi_list_saved_json() const {
    std::string error;
    std::vector<WifiSavedNetwork> records = wifi_list_saved(wifi_iface_, error);
    if (!error.empty() && records.empty()) {
        return error_json(500, error);
    }
    return ok_json(wifi_saved_to_json(records));
}

std::string NetworkDaemon::wifi_forget_json(const std::string &ssid) const {
    std::string error;
    if (!wifi_forget_saved(wifi_iface_, ssid, error)) {
        return error_json(500, error);
    }
    return ok_json("{\"requested\":\"forget\"}");
}

std::string NetworkDaemon::wifi_set_autoconnect_json(const std::string &ssid, bool enabled) const {
    std::string error;
    if (!wifi_set_autoconnect(wifi_iface_, ssid, enabled, error)) {
        return error_json(500, error);
    }
    return ok_json(std::string("{\"requested\":\"autoconnect\",\"enabled\":") +
                   (enabled ? "true" : "false") + "}");
}

std::string NetworkDaemon::wifi_disconnect_json() const {
    if (wifi_manager_) {
        wifi_manager_->stop_dhcp();
    }
    std::string error;
    if (!wifi_disconnect(wifi_iface_, error)) {
        return error_json(500, error);
    }
    return ok_json("{\"requested\":\"disconnect\"}");
}

} // namespace network_service
