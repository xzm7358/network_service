#include "platform/wpa_event_monitor.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <poll.h>
#include <sstream>
#include <utility>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "platform/wifi_backend.h"

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

static bool send_ctrl(int fd, const char *cmd) {
    return send(fd, cmd, strlen(cmd), 0) == static_cast<ssize_t>(strlen(cmd));
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

static std::string ctrl_path_for(const std::string &dir, const std::string &iface) {
    std::string base = dir.empty() ? "/var/run/wpa_supplicant" : dir;
    if (!base.empty() && base.back() == '/') base.pop_back();
    return base + "/" + iface;
}

static const char *semantic_state_for_event(const std::string &event) {
    if (event.find("CTRL-EVENT-SCAN-STARTED") != std::string::npos) return "scanning";
    if (event.find("Trying to associate") != std::string::npos) return "associating";
    if (event.find("Associated with") != std::string::npos) return "associated";
    if (event.find("4-Way Handshake") != std::string::npos ||
        event.find("Key negotiation completed") != std::string::npos ||
        event.find("WPA: Key negotiation") != std::string::npos) return "handshake";
    if (event.find("CTRL-EVENT-CONNECTED") != std::string::npos) return "ip_configuring";
    if (event.find("CTRL-EVENT-DISCONNECTED") != std::string::npos) return "disconnected";
    if (event.find("CTRL-EVENT-SSID-TEMP-DISABLED") != std::string::npos ||
        event.find("CTRL-EVENT-ASSOC-REJECT") != std::string::npos ||
        event.find("CTRL-EVENT-AUTH-REJECT") != std::string::npos ||
        event.find("WRONG_KEY") != std::string::npos) return "failed";
    return nullptr;
}

static const char *failure_reason_for_event(const std::string &event) {
    if (event.find("WRONG_KEY") != std::string::npos) return "wrong_key";
    if (event.find("CTRL-EVENT-SSID-TEMP-DISABLED") != std::string::npos) return "ssid_temp_disabled";
    if (event.find("CTRL-EVENT-ASSOC-REJECT") != std::string::npos) return "assoc_reject";
    if (event.find("CTRL-EVENT-AUTH-REJECT") != std::string::npos) return "auth_reject";
    return "";
}

} // namespace

WpaEventMonitor::WpaEventMonitor(std::string iface, std::string ctrl_dir)
    : iface_(std::move(iface)), ctrl_dir_(std::move(ctrl_dir)) {}

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

void WpaEventMonitor::update_event(const std::string &event) {
    std::lock_guard<std::mutex> guard(lock_);
    snapshot_.last_event = event;
    if (event.find("CTRL-EVENT-SCAN-STARTED") != std::string::npos) {
        snapshot_.scan_started_events++;
    }
    if (event.find("CTRL-EVENT-SCAN-RESULTS") != std::string::npos) {
        snapshot_.scan_result_events++;
    }
    if (event.find("CTRL-EVENT-SCAN-FAILED") != std::string::npos) {
        snapshot_.scan_failed_events++;
    }
    if (const char *semantic = semantic_state_for_event(event)) {
        snapshot_.wifi_state = semantic;
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
        snapshot_.dhcp_requested = false;
        snapshot_.has_ip = false;
        snapshot_.has_default_route = false;
        snapshot_.dns_available = false;
        snapshot_.ip4.clear();
        snapshot_.gateway4.clear();
    } else if (snapshot_.wifi_state == "failed") {
        snapshot_.connected = false;
        snapshot_.disconnected = false;
        snapshot_.failure_reason = failure_reason_for_event(event);
    }
}

bool WpaEventMonitor::handle_connected_event() {
    std::string error;
    bool ok = wifi_start_dhcp(iface_, error);
    std::lock_guard<std::mutex> guard(lock_);
    snapshot_.dhcp_requests++;
    snapshot_.dhcp_requested = ok;
    snapshot_.wifi_state = ok ? "ip_configuring" : "failed";
    if (!ok) {
        snapshot_.failure_reason = "dhcp_start_failed";
        snapshot_.last_event += " dhcp_error=" + error;
    }
    return ok;
}

void WpaEventMonitor::run() {
    while (running_) {
        int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
        if (fd < 0) {
            usleep(1000 * 1000);
            continue;
        }

        std::string local_path = "/tmp/network_service_wpa_" + iface_ + "_" + std::to_string(getpid()) + ".sock";
        unlink(local_path.c_str());

        sockaddr_un local;
        memset(&local, 0, sizeof(local));
        local.sun_family = AF_UNIX;
        snprintf(local.sun_path, sizeof(local.sun_path), "%s", local_path.c_str());
        if (bind(fd, reinterpret_cast<sockaddr *>(&local), sizeof(local)) != 0) {
            close(fd);
            usleep(1000 * 1000);
            continue;
        }

        sockaddr_un remote;
        memset(&remote, 0, sizeof(remote));
        remote.sun_family = AF_UNIX;
        std::string ctrl_path = ctrl_path_for(ctrl_dir_, iface_);
        snprintf(remote.sun_path, sizeof(remote.sun_path), "%s", ctrl_path.c_str());
        if (connect(fd, reinterpret_cast<sockaddr *>(&remote), sizeof(remote)) != 0) {
            close(fd);
            unlink(local_path.c_str());
            usleep(1500 * 1000);
            continue;
        }

        if (!send_ctrl(fd, "ATTACH")) {
            close(fd);
            unlink(local_path.c_str());
            usleep(1500 * 1000);
            continue;
        }

        char buffer[1024];
        ssize_t n = recv(fd, buffer, sizeof(buffer) - 1, 0);
        if (n <= 0) {
            close(fd);
            unlink(local_path.c_str());
            usleep(1500 * 1000);
            continue;
        }
        buffer[n] = '\0';
        {
            std::lock_guard<std::mutex> guard(lock_);
            snapshot_.attached = strstr(buffer, "OK") != nullptr;
        }

        while (running_) {
            struct pollfd pfd;
            pfd.fd = fd;
            pfd.events = POLLIN;
            pfd.revents = 0;
            int ret = poll(&pfd, 1, 1000);
            if (ret < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (ret == 0) continue;
            if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) break;
            if (!(pfd.revents & POLLIN)) continue;

            n = recv(fd, buffer, sizeof(buffer) - 1, 0);
            if (n <= 0) break;
            buffer[n] = '\0';
            std::string event(buffer);
            std::string normalized = normalize_event(event);
            update_event(normalized);
            if (normalized.find("CTRL-EVENT-CONNECTED") != std::string::npos) {
                handle_connected_event();
            }
        }

        (void)send_ctrl(fd, "DETACH");
        close(fd);
        unlink(local_path.c_str());
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
       << "\"wifi_state\":\"" << json_escape(snapshot.wifi_state) << "\","
       << "\"failure_reason\":\"" << json_escape(snapshot.failure_reason) << "\","
       << "\"last_event\":\"" << json_escape(snapshot.last_event) << "\","
       << "\"last_ssid\":\"" << json_escape(snapshot.last_ssid) << "\","
       << "\"last_bssid\":\"" << json_escape(snapshot.last_bssid) << "\""
       << "}";
    return os.str();
}

} // namespace network_service
