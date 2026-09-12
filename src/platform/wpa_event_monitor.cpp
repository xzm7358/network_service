#include "platform/wpa_event_monitor.h"

#include <cerrno>
#include <poll.h>
#include <sstream>
#include <utility>
#include <unistd.h>

#include "platform/wpa_ctrl_client.h"

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

static std::string field_after(const std::string &event, const char *key) {
    std::string needle = key;
    size_t pos = event.find(needle);
    if (pos == std::string::npos) return {};
    pos += needle.size();
    size_t end = event.find(' ', pos);
    if (end == std::string::npos) end = event.size();
    return event.substr(pos, end - pos);
}

static std::string normalize_event(const std::string &event) {
    size_t pos = event.find("CTRL-EVENT-");
    if (pos == std::string::npos) {
        return event;
    }
    return event.substr(pos);
}

static bool l2_state_for_event(const std::string &event, WifiL2State &state) {
    if (event.find("Trying to associate") != std::string::npos) {
        state = WifiL2State::Associating;
        return true;
    }
    if (event.find("Associated with") != std::string::npos) {
        state = WifiL2State::Associated;
        return true;
    }
    if (event.find("4-Way Handshake") != std::string::npos ||
        event.find("Key negotiation completed") != std::string::npos ||
        event.find("WPA: Key negotiation") != std::string::npos) {
        state = WifiL2State::Handshake;
        return true;
    }
    if (event.find("CTRL-EVENT-CONNECTED") != std::string::npos) {
        state = WifiL2State::Connected;
        return true;
    }
    if (event.find("CTRL-EVENT-DISCONNECTED") != std::string::npos) {
        state = WifiL2State::Disconnected;
        return true;
    }
    if (event.find("CTRL-EVENT-SSID-TEMP-DISABLED") != std::string::npos ||
        event.find("CTRL-EVENT-ASSOC-REJECT") != std::string::npos ||
        event.find("CTRL-EVENT-AUTH-REJECT") != std::string::npos ||
        event.find("WRONG_KEY") != std::string::npos) {
        state = WifiL2State::Failed;
        return true;
    }
    return false;
}

static const char *legacy_state_for_l2(WifiL2State state) {
    switch (state) {
    case WifiL2State::Disabled: return "disabled";
    case WifiL2State::Disconnected: return "disconnected";
    case WifiL2State::Associating: return "associating";
    case WifiL2State::Associated: return "associated";
    case WifiL2State::Handshake: return "handshake";
    case WifiL2State::Connected: return "ip_configuring";
    case WifiL2State::Failed: return "failed";
    case WifiL2State::Unknown:
    default: return "disconnected";
    }
}

static const char *failure_reason_for_event(const std::string &event) {
    if (event.find("WRONG_KEY") != std::string::npos) return "wrong_key";
    if (event.find("CTRL-EVENT-SSID-TEMP-DISABLED") != std::string::npos) return "ssid_temp_disabled";
    if (event.find("CTRL-EVENT-ASSOC-REJECT") != std::string::npos) return "assoc_reject";
    if (event.find("CTRL-EVENT-AUTH-REJECT") != std::string::npos) return "auth_reject";
    return "";
}

} // namespace

WpaEventMonitor::WpaEventMonitor(std::string iface,
                                 std::string ctrl_dir,
                                 LinkStateHandler link_state_handler)
    : iface_(std::move(iface)),
      ctrl_dir_(std::move(ctrl_dir)),
      link_state_handler_(std::move(link_state_handler)) {}

WpaEventMonitor::~WpaEventMonitor() {
    stop();
}

void WpaEventMonitor::start() {
    if (running_.exchange(true)) {
        return;
    }
    thread_ = std::thread(&WpaEventMonitor::run, this);
}

void WpaEventMonitor::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    if (thread_.joinable()) {
        thread_.join();
    }
}

WpaEventSnapshot WpaEventMonitor::snapshot() const {
    std::lock_guard<std::mutex> guard(lock_);
    return snapshot_;
}

WifiL2State WpaEventMonitor::update_event(const std::string &event, bool &l2_changed) {
    std::lock_guard<std::mutex> guard(lock_);
    l2_changed = false;
    snapshot_.last_event = event;
    ++snapshot_.event_sequence;
    if (snapshot_.event_sequence == 0) ++snapshot_.event_sequence;
    const uint64_t sequence = snapshot_.event_sequence;

    if (event.find("CTRL-EVENT-SCAN-STARTED") != std::string::npos) {
        snapshot_.scan_active = true;
        snapshot_.scan_started_events++;
        snapshot_.last_scan_started_sequence = sequence;
        if (!snapshot_.connected) snapshot_.wifi_state = "scanning";
    }
    if (event.find("CTRL-EVENT-SCAN-RESULTS") != std::string::npos) {
        snapshot_.scan_active = false;
        snapshot_.scan_result_events++;
        snapshot_.last_scan_result_sequence = sequence;
        snapshot_.wifi_state = legacy_state_for_l2(snapshot_.l2_state);
    }
    if (event.find("CTRL-EVENT-SCAN-FAILED") != std::string::npos) {
        snapshot_.scan_active = false;
        snapshot_.scan_failed_events++;
        snapshot_.last_scan_failed_sequence = sequence;
        snapshot_.wifi_state = legacy_state_for_l2(snapshot_.l2_state);
    }

    WifiL2State l2 = WifiL2State::Unknown;
    if (l2_state_for_event(event, l2)) {
        l2_changed = snapshot_.l2_state != l2;
        snapshot_.l2_state = l2;
        snapshot_.wifi_state = legacy_state_for_l2(l2);
    }

    if (event.find("CTRL-EVENT-CONNECTED") != std::string::npos) {
        snapshot_.connected = true;
        snapshot_.disconnected = false;
        snapshot_.failure_reason.clear();
        snapshot_.connect_events++;
        std::string id = field_after(event, "id_str=");
        if (!id.empty()) snapshot_.last_ssid = id;
        std::string bssid = field_after(event, "Connection to ");
        if (!bssid.empty()) snapshot_.last_bssid = bssid;
    } else if (event.find("CTRL-EVENT-DISCONNECTED") != std::string::npos) {
        snapshot_.connected = false;
        snapshot_.disconnected = true;
        snapshot_.disconnect_events++;
    } else if (snapshot_.l2_state == WifiL2State::Failed) {
        snapshot_.connected = false;
        snapshot_.disconnected = false;
        snapshot_.failure_reason = failure_reason_for_event(event);
    }
    return snapshot_.l2_state;
}

void WpaEventMonitor::run() {
    const std::string ctrl_path = wpa_ctrl_path_for(iface_, ctrl_dir_);

    while (running_) {
        WpaCtrlClient ctrl(ctrl_path);
        std::string error;
        if (!ctrl.open(error) || !ctrl.attach(error)) {
            {
                std::lock_guard<std::mutex> guard(lock_);
                snapshot_.attached = false;
            }
            usleep(1500 * 1000);
            continue;
        }

        {
            std::lock_guard<std::mutex> guard(lock_);
            snapshot_.attached = true;
        }

        while (running_) {
            pollfd pfd{};
            pfd.fd = ctrl.fd();
            pfd.events = POLLIN;
            int ret = poll(&pfd, 1, 1000);
            if (ret < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (ret == 0) continue;
            if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) break;
            if (!(pfd.revents & POLLIN)) continue;

            std::string event;
            if (!ctrl.receive(event, error)) break;
            const std::string normalized = normalize_event(event);
            bool l2_changed = false;
            const WifiL2State l2_state = update_event(normalized, l2_changed);

            if (l2_changed && link_state_handler_) {
                link_state_handler_(l2_state);
            }
        }

        {
            std::lock_guard<std::mutex> guard(lock_);
            snapshot_.attached = false;
        }
        ctrl.close();
        if (running_) {
            usleep(1000 * 1000);
        }
    }
}

std::string wpa_event_snapshot_to_json(const WpaEventSnapshot &snapshot) {
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

} // namespace network_service
