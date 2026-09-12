#include "ipc/network_ipc_representation.h"

#include <sstream>

#ifndef NETWORK_SERVICE_VERSION
#define NETWORK_SERVICE_VERSION "0.1.0"
#endif

namespace network_service {
namespace ipc_representation {

namespace {

const char *route_policy_to_string(RoutePolicy policy) {
    switch (policy) {
    case RoutePolicy::WifiPreferred: return "wifi_preferred";
    case RoutePolicy::WifiOnly: return "wifi_only";
    case RoutePolicy::ManualMetric: return "manual_metric";
    case RoutePolicy::EthernetPreferred:
    default: return "ethernet_preferred";
    }
}

const char *dns_policy_to_string(DnsPolicy policy) {
    switch (policy) {
    case DnsPolicy::Append: return "append";
    case DnsPolicy::Disabled: return "disabled";
    case DnsPolicy::Overwrite:
    default: return "overwrite";
    }
}

std::string iface_payload(const InterfaceSnapshot &iface) {
    std::ostringstream os;
    os << "{"
       << "\"iface\":\"" << json_escape(iface.iface) << "\","
       << "\"exists\":" << (iface.exists ? "true" : "false") << ","
       << "\"carrier_up\":" << (iface.carrier_up ? "true" : "false") << ","
       << "\"enabled\":" << (iface.enabled ? "true" : "false") << ","
       << "\"connected\":" << (iface.connected ? "true" : "false") << ","
       << "\"has_ip\":" << (iface.has_ip ? "true" : "false") << ","
       << "\"ip4\":\"" << json_escape(iface.ip4) << "\","
       << "\"netmask4\":\"" << json_escape(iface.netmask4) << "\","
       << "\"has_default_route\":" << (iface.has_default_route ? "true" : "false") << ","
       << "\"gateway4\":\"" << json_escape(iface.gateway4) << "\","
       << "\"route_metric\":" << iface.route_metric << ","
       << "\"ssid\":\"" << json_escape(iface.ssid) << "\","
       << "\"signal_dbm\":" << iface.signal_dbm << ","
       << "\"signal_bars\":" << iface.signal_bars
       << "}";
    return os.str();
}

} // namespace

std::string json_escape(const std::string &value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (unsigned char ch : value) {
        switch (ch) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (ch < 0x20U) {
                static const char hex[] = "0123456789abcdef";
                out += "\\u00";
                out += hex[(ch >> 4U) & 0x0fU];
                out += hex[ch & 0x0fU];
            } else {
                out.push_back(static_cast<char>(ch));
            }
            break;
        }
    }
    return out;
}

std::string snapshot_payload(const NetworkSnapshot &snapshot) {
    std::ostringstream os;
    os << "{"
       << "\"version\":\"" << NETWORK_SERVICE_VERSION << "\","
       << "\"primary_iface\":\"" << json_escape(snapshot.primary_iface) << "\","
       << "\"online\":" << (snapshot.online ? "true" : "false") << ","
       << "\"dns_available\":" << (snapshot.dns_available ? "true" : "false") << ","
       << "\"dns4\":\"" << json_escape(snapshot.dns4) << "\","
       << "\"route_policy\":\"" << route_policy_to_string(snapshot.route_policy) << "\","
       << "\"dns_policy\":\"" << dns_policy_to_string(snapshot.dns_policy) << "\","
       << "\"eth\":" << iface_payload(snapshot.eth) << ","
       << "\"wifi\":" << iface_payload(snapshot.wifi)
       << "}";
    return os.str();
}

std::string ping_payload(const PingInfo &info) {
    std::ostringstream os;
    os << "{\"service\":\"" << json_escape(info.service)
       << "\",\"version\":\"" << json_escape(info.version)
       << "\",\"mode\":\"" << json_escape(info.mode) << "\"}";
    return os.str();
}

std::string wpa_events_payload(const WpaEventSnapshot &snapshot) {
    std::ostringstream os;
    os << "{"
       << "\"attached\":" << (snapshot.attached ? "true" : "false") << ","
       << "\"connected\":" << (snapshot.connected ? "true" : "false") << ","
       << "\"disconnected\":" << (snapshot.disconnected ? "true" : "false") << ","
       << "\"dhcp_requested\":" << (snapshot.dhcp_requested ? "true" : "false") << ","
       << "\"has_ip\":" << (snapshot.has_ip ? "true" : "false") << ","
       << "\"has_default_route\":" << (snapshot.has_default_route ? "true" : "false") << ","
       << "\"dns_available\":" << (snapshot.dns_available ? "true" : "false") << ","
       << "\"ip4\":\"" << json_escape(snapshot.ip4) << "\","
       << "\"gateway4\":\"" << json_escape(snapshot.gateway4) << "\","
       << "\"dns4\":\"" << json_escape(snapshot.dns4) << "\","
       << "\"connect_events\":" << snapshot.connect_events << ","
       << "\"disconnect_events\":" << snapshot.disconnect_events << ","
       << "\"dhcp_requests\":" << snapshot.dhcp_requests << ","
       << "\"scan_started_events\":" << snapshot.scan_started_events << ","
       << "\"scan_result_events\":" << snapshot.scan_result_events << ","
       << "\"scan_failed_events\":" << snapshot.scan_failed_events << ","
       << "\"event_sequence\":" << snapshot.event_sequence << ","
       << "\"last_scan_started_sequence\":" << snapshot.last_scan_started_sequence << ","
       << "\"last_scan_result_sequence\":" << snapshot.last_scan_result_sequence << ","
       << "\"last_scan_failed_sequence\":" << snapshot.last_scan_failed_sequence << ","
       << "\"wifi_state\":\"" << json_escape(snapshot.wifi_state) << "\","
       << "\"failure_reason\":\"" << json_escape(snapshot.failure_reason) << "\","
       << "\"last_event\":\"" << json_escape(snapshot.last_event) << "\","
       << "\"last_ssid\":\"" << json_escape(snapshot.last_ssid) << "\","
       << "\"last_bssid\":\"" << json_escape(snapshot.last_bssid) << "\""
       << "}";
    return os.str();
}

std::string ethernet_config_payload(const EthernetConfig &config) {
    std::ostringstream os;
    os << "{"
       << "\"iface\":\"" << json_escape(config.iface) << "\","
       << "\"method\":\"" << json_escape(config.method) << "\","
       << "\"ip4\":\"" << json_escape(config.ip4) << "\","
       << "\"netmask4\":\"" << json_escape(config.netmask4) << "\","
       << "\"gateway4\":\"" << json_escape(config.gateway4) << "\","
       << "\"dns4\":\"" << json_escape(config.dns4) << "\","
       << "\"route_metric\":" << config.route_metric << ","
       << "\"dns_enabled\":" << (config.dns_enabled ? "true" : "false")
       << "}";
    return os.str();
}

std::string wifi_scan_payload(const std::vector<WifiApRecord> &records) {
    std::ostringstream os;
    os << "{\"count\":" << records.size() << ",\"aps\":[";
    for (std::size_t i = 0; i < records.size(); ++i) {
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

std::string wifi_saved_payload(const std::vector<WifiSavedNetwork> &records) {
    std::ostringstream os;
    os << "{\"count\":" << records.size() << ",\"saved\":[";
    for (std::size_t i = 0; i < records.size(); ++i) {
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

std::string wifi_scan_status_payload(const WifiScanStatus &status) {
    std::ostringstream os;
    os << "{\"scanId\":" << status.scan_id
       << ",\"state\":\"" << wifi_scan_state_name(status.state) << "\""
       << ",\"error\":\"" << json_escape(status.error) << "\""
       << ",\"results\":" << wifi_scan_payload(status.records)
       << "}";
    return os.str();
}

std::string wifi_enabled_payload(const WifiEnabledResult &result) {
    return std::string("{\"enabled\":") + (result.enabled ? "true" : "false") + "}";
}

std::string wifi_command_payload(const WifiCommandResult &result) {
    std::ostringstream os;
    os << "{\"requested\":\"" << json_escape(result.requested) << "\"";
    if (result.has_enabled) {
        os << ",\"enabled\":" << (result.enabled ? "true" : "false");
    }
    os << "}";
    return os.str();
}

std::string v0_success(int status, const std::string &payload_json) {
    std::ostringstream os;
    os << "{\"status\":" << status << ",\"result\":" << payload_json << "}";
    return os.str();
}

std::string v0_error(int status, const std::string &message) {
    std::ostringstream os;
    os << "{\"status\":" << status << ",\"error\":\""
       << json_escape(message) << "\"}";
    return os.str();
}

} // namespace ipc_representation
} // namespace network_service
