#include "platform/wpa_event_monitor.h"

#include <cerrno>
#include <poll.h>
#include <sstream>
#include <utility>
#include <unistd.h>

#include "platform/wpa_ctrl_client.h"
#include "platform/wpa_text_codec.h"

namespace network_service {

namespace {

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
    if (pos == std::string::npos) return event;
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

static const char *failure_reason_for_event(const std::string &event) {
    if (event.find("WRONG_KEY") != std::string::npos) return "wrong_key";
    if (event.find("CTRL-EVENT-SSID-TEMP-DISABLED") != std::string::npos)
        return "ssid_temp_disabled";
    if (event.find("CTRL-EVENT-ASSOC-REJECT") != std::string::npos)
        return "assoc_reject";
    if (event.find("CTRL-EVENT-AUTH-REJECT") != std::string::npos)
        return "auth_reject";
    return "";
}

static WifiL2State l2_state_for_status(const std::string &value) {
    if (value == "COMPLETED") return WifiL2State::Connected;
    if (value == "ASSOCIATING") return WifiL2State::Associating;
    if (value == "ASSOCIATED") return WifiL2State::Associated;
    if (value == "4WAY_HANDSHAKE" || value == "GROUP_HANDSHAKE") {
        return WifiL2State::Handshake;
    }
    if (value == "INTERFACE_DISABLED") return WifiL2State::Disabled;
    if (value == "DISCONNECTED" || value == "INACTIVE" || value == "SCANNING") {
        return WifiL2State::Disconnected;
    }
    return WifiL2State::Unknown;
}

static void bump_sequence(WpaEventFact &snapshot) {
    ++snapshot.event_sequence;
    if (snapshot.event_sequence == 0) ++snapshot.event_sequence;
}

} // namespace

bool parse_wpa_status_reply(const std::string &reply, WpaStatusFact &fact) {
    fact = WpaStatusFact{};
    std::istringstream input(reply);
    std::string line;
    bool have_state = false;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::size_t pos = line.find('=');
        if (pos == std::string::npos) continue;
        const std::string key = line.substr(0, pos);
        const std::string value = line.substr(pos + 1);
        if (key == "wpa_state") {
            fact.l2_state = l2_state_for_status(value);
            have_state = true;
        } else if (key == "ssid") {
            fact.ssid = decode_wpa_printable_text(value);
        } else if (key == "bssid") {
            fact.bssid = value;
        }
    }
    return have_state;
}

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
    if (running_.exchange(true)) return;
    thread_ = std::thread(&WpaEventMonitor::run, this);
}

void WpaEventMonitor::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
}

WpaEventFact WpaEventMonitor::snapshot() const {
    std::lock_guard<std::mutex> guard(lock_);
    return snapshot_;
}

void WpaEventMonitor::apply_status(const WpaStatusFact &status) {
    std::lock_guard<std::mutex> guard(lock_);
    snapshot_.attached = true;
    snapshot_.l2_state = status.l2_state;
    snapshot_.failure_reason.clear();
    if (!status.ssid.empty()) snapshot_.last_ssid = status.ssid;
    if (!status.bssid.empty()) snapshot_.last_bssid = status.bssid;
    bump_sequence(snapshot_);
}

void WpaEventMonitor::mark_channel_unavailable() {
    std::lock_guard<std::mutex> guard(lock_);
    const bool changed = snapshot_.attached ||
                         snapshot_.l2_state != WifiL2State::Unknown ||
                         snapshot_.scan_active ||
                         !snapshot_.failure_reason.empty();
    snapshot_.attached = false;
    // Losing the ctrl channel is not evidence of DISCONNECTED. Explicitly drop
    // previously observed L2 truth to Unknown so stale Connected/Disconnected
    // state is never treated as authoritative after a monitor failure.
    snapshot_.l2_state = WifiL2State::Unknown;
    snapshot_.scan_active = false;
    snapshot_.failure_reason.clear();
    if (changed) bump_sequence(snapshot_);
}

void WpaEventMonitor::update_event(const std::string &event) {
    std::lock_guard<std::mutex> guard(lock_);
    snapshot_.last_event = event;
    bump_sequence(snapshot_);
    const std::uint64_t sequence = snapshot_.event_sequence;

    if (event.find("CTRL-EVENT-SCAN-STARTED") != std::string::npos) {
        snapshot_.scan_active = true;
        snapshot_.scan_started_events++;
        snapshot_.last_scan_started_sequence = sequence;
    }
    if (event.find("CTRL-EVENT-SCAN-RESULTS") != std::string::npos) {
        snapshot_.scan_active = false;
        snapshot_.scan_result_events++;
        snapshot_.last_scan_result_sequence = sequence;
    }
    if (event.find("CTRL-EVENT-SCAN-FAILED") != std::string::npos) {
        snapshot_.scan_active = false;
        snapshot_.scan_failed_events++;
        snapshot_.last_scan_failed_sequence = sequence;
    }

    WifiL2State l2 = WifiL2State::Unknown;
    if (l2_state_for_event(event, l2)) snapshot_.l2_state = l2;

    if (event.find("CTRL-EVENT-CONNECTED") != std::string::npos) {
        snapshot_.failure_reason.clear();
        snapshot_.connect_events++;
        std::string id = field_after(event, "id_str=");
        if (!id.empty()) snapshot_.last_ssid = id;
        std::string bssid = field_after(event, "Connection to ");
        if (!bssid.empty()) snapshot_.last_bssid = bssid;
    } else if (event.find("CTRL-EVENT-DISCONNECTED") != std::string::npos) {
        snapshot_.disconnect_events++;
    } else if (snapshot_.l2_state == WifiL2State::Failed) {
        snapshot_.failure_reason = failure_reason_for_event(event);
    }
}

void WpaEventMonitor::run() {
    const std::string ctrl_path = wpa_ctrl_path_for(iface_, ctrl_dir_);

    while (running_) {
        WpaCtrlClient ctrl(ctrl_path);
        std::string error;
        if (!ctrl.open(error) || !ctrl.attach(error)) {
            mark_channel_unavailable();
            usleep(1500 * 1000);
            continue;
        }

        // Never issue STATUS on the attached event socket: unsolicited
        // CTRL-EVENT frames may otherwise be consumed as the request reply. A
        // short-lived independent ctrl client gives us an authoritative current
        // state while the attached socket continues to queue transitions.
        WpaStatusFact status;
        std::string status_reply;
        std::string status_error;
        WpaCtrlClient status_ctrl(ctrl_path);
        const bool status_ok = status_ctrl.request("STATUS", status_reply, status_error) &&
                               parse_wpa_status_reply(status_reply, status);
        status_ctrl.close();

        if (status_ok) {
            apply_status(status);
            if (link_state_handler_ && status.l2_state != WifiL2State::Unknown) {
                link_state_handler_(status.l2_state == WifiL2State::Connected);
            }
        } else {
            // ATTACH succeeded, but no point-in-time status was available. Keep
            // monitor attachment truth while refusing to reuse a stale L2 fact.
            {
                std::lock_guard<std::mutex> guard(lock_);
                snapshot_.attached = true;
                snapshot_.l2_state = WifiL2State::Unknown;
                snapshot_.failure_reason.clear();
                bump_sequence(snapshot_);
            }
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
            update_event(normalized);

            if (!link_state_handler_) continue;
            if (normalized.find("CTRL-EVENT-CONNECTED") != std::string::npos) {
                link_state_handler_(true);
            } else if (normalized.find("CTRL-EVENT-DISCONNECTED") != std::string::npos) {
                link_state_handler_(false);
            }
        }

        mark_channel_unavailable();
        ctrl.close();
        if (running_) usleep(1000 * 1000);
    }
}

} // namespace network_service
