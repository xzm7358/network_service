#include "ipc/network_ipc_server.h"

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "network_service_protocol.h"
#include "service/network_daemon.h"

namespace network_service {

namespace {

static int g_wake_fd = -1;
constexpr int kClientIdleTimeoutMs = 1000;
constexpr size_t kMaxRequestBytes = 64 * 1024;

static std::string json_unescape(const std::string &value) {
    std::string out;
    out.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] != '\\' || i + 1 >= value.size()) {
            out += value[i];
            continue;
        }
        char next = value[++i];
        switch (next) {
        case '\\': out += '\\'; break;
        case '"': out += '"'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        default: out += next; break;
        }
    }
    return out;
}

static std::string extract_json_string(const std::string &request, const char *field) {
    std::string key = "\"";
    key += field;
    key += "\"";
    size_t pos = request.find(key);
    if (pos == std::string::npos) return {};
    pos = request.find(':', pos + key.size());
    if (pos == std::string::npos) return {};
    pos = request.find('"', pos + 1);
    if (pos == std::string::npos) return {};
    std::string raw;
    bool escaped = false;
    for (size_t i = pos + 1; i < request.size(); ++i) {
        char ch = request[i];
        if (escaped) {
            raw += '\\';
            raw += ch;
            escaped = false;
            continue;
        }
        if (ch == '\\') {
            escaped = true;
            continue;
        }
        if (ch == '"') {
            return json_unescape(raw);
        }
        raw += ch;
    }
    return {};
}

static std::string extract_method(const std::string &request) {
    return extract_json_string(request, "method");
}

static bool send_all(int fd, const std::string &data) {
    const char *p = data.data();
    size_t remaining = data.size();
    while (remaining > 0) {
        ssize_t written = send(fd, p, remaining, 0);
        if (written < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (written == 0) return false;
        p += written;
        remaining -= static_cast<size_t>(written);
    }
    return true;
}

} // namespace

NetworkIpcServer::NetworkIpcServer(NetworkDaemon &daemon)
    : daemon_(daemon) {
    int pipefd[2] = {-1, -1};
    if (pipe(pipefd) == 0) {
        wake_read_fd_ = pipefd[0];
        wake_write_fd_ = pipefd[1];
        int flags = fcntl(wake_read_fd_, F_GETFL, 0);
        if (flags >= 0) {
            (void)fcntl(wake_read_fd_, F_SETFL, flags | O_NONBLOCK);
        }
    }
}

NetworkIpcServer::~NetworkIpcServer() {
    stop();
    if (wake_read_fd_ >= 0) {
        close(wake_read_fd_);
        wake_read_fd_ = -1;
    }
    if (wake_write_fd_ >= 0) {
        close(wake_write_fd_);
        wake_write_fd_ = -1;
    }
}

int NetworkIpcServer::wake_write_fd() const {
    return wake_write_fd_;
}

int NetworkIpcServer::wake_fd() {
    return g_wake_fd;
}

void NetworkIpcServer::set_wake_fd(int fd) {
    g_wake_fd = fd;
}

bool NetworkIpcServer::listen(const std::string &socket_path) {
    socket_path_ = socket_path;
    listen_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        std::cerr << "network_service: socket failed: " << strerror(errno) << std::endl;
        return false;
    }

    sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (socket_path_.size() >= sizeof(addr.sun_path)) {
        std::cerr << "network_service: socket path too long: " << socket_path_ << std::endl;
        close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", socket_path_.c_str());

    unlink(socket_path_.c_str());
    if (bind(listen_fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        std::cerr << "network_service: bind " << socket_path_ << " failed: " << strerror(errno) << std::endl;
        close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    chmod(socket_path_.c_str(), 0666);

    if (::listen(listen_fd_, 8) != 0) {
        std::cerr << "network_service: listen failed: " << strerror(errno) << std::endl;
        close(listen_fd_);
        listen_fd_ = -1;
        unlink(socket_path_.c_str());
        return false;
    }

    running_ = true;
    return true;
}

void NetworkIpcServer::run() {
    while (running_) {
        struct pollfd fds[2];
        fds[0].fd = listen_fd_;
        fds[0].events = POLLIN;
        fds[0].revents = 0;
        nfds_t poll_count = 1;
        if (wake_read_fd_ >= 0) {
            fds[1].fd = wake_read_fd_;
            fds[1].events = POLLIN;
            fds[1].revents = 0;
            poll_count = 2;
        }

        int ret = poll(fds, poll_count, -1);
        if (ret < 0) {
            if (errno == EINTR) continue;
            std::cerr << "network_service: poll failed: " << strerror(errno) << std::endl;
            continue;
        }
        if (poll_count > 1 && (fds[1].revents & (POLLIN | POLLERR | POLLHUP | POLLNVAL))) {
            char drain[32];
            while (read(wake_read_fd_, drain, sizeof(drain)) > 0) {}
            running_ = false;
            break;
        }
        if (!(fds[0].revents & POLLIN)) {
            if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                running_ = false;
                break;
            }
            continue;
        }

        int client_fd = accept(listen_fd_, nullptr, nullptr);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            if (!running_ || listen_fd_ < 0) break;
            std::cerr << "network_service: accept failed: " << strerror(errno) << std::endl;
            continue;
        }
        handle_client(client_fd);
        close(client_fd);
    }
}

void NetworkIpcServer::stop() {
    running_ = false;
    if (listen_fd_ >= 0) {
        shutdown(listen_fd_, SHUT_RDWR);
        close(listen_fd_);
        listen_fd_ = -1;
    }
    if (!socket_path_.empty()) {
        unlink(socket_path_.c_str());
    }
}

std::string NetworkIpcServer::handle_request(const std::string &request) {
    std::string method = extract_method(request);
    if (method == kMethodPing) {
        return daemon_.ping_json() + "\n";
    }
    if (method == kMethodSnapshot || method.empty()) {
        return daemon_.snapshot_json() + "\n";
    }
    if (method == "eth.get_config") {
        return daemon_.eth_get_config_json() + "\n";
    }
    if (method == "eth.set_dhcp") {
        return daemon_.eth_set_dhcp_json() + "\n";
    }
    if (method == "eth.set_static") {
        return daemon_.eth_set_static_json(extract_json_string(request, "ip"),
                                           extract_json_string(request, "mask"),
                                           extract_json_string(request, "gateway"),
                                           extract_json_string(request, "dns")) + "\n";
    }
    if (method == "wpa.events") {
        return daemon_.wpa_events_json() + "\n";
    }
    if (method == "wifi.scan") {
        return daemon_.wifi_scan_json() + "\n";
    }
    if (method == "wifi.set_enabled") {
        std::string enabled = extract_json_string(request, "enabled");
        return daemon_.wifi_set_enabled_json(enabled == "1" || enabled == "true") + "\n";
    }
    if (method == "wifi.connect") {
        return daemon_.wifi_connect_json(extract_json_string(request, "ssid"),
                                         extract_json_string(request, "password")) + "\n";
    }
    if (method == "wifi.connect_saved") {
        return daemon_.wifi_connect_saved_json(extract_json_string(request, "ssid")) + "\n";
    }
    if (method == "wifi.saved_list") {
        return daemon_.wifi_list_saved_json() + "\n";
    }
    if (method == "wifi.forget") {
        return daemon_.wifi_forget_json(extract_json_string(request, "ssid")) + "\n";
    }
    if (method == "wifi.autoconnect") {
        std::string enabled = extract_json_string(request, "enabled");
        return daemon_.wifi_set_autoconnect_json(extract_json_string(request, "ssid"),
                                                 enabled == "1" || enabled == "true") + "\n";
    }
    if (method == "wifi.disconnect") {
        return daemon_.wifi_disconnect_json() + "\n";
    }
    return "{\"status\":404,\"error\":\"unknown method\"}\n";
}

void NetworkIpcServer::handle_client(int client_fd) {
    std::string request;
    char buffer[1024];
    while (true) {
        struct pollfd fds[2];
        fds[0].fd = client_fd;
        fds[0].events = POLLIN;
        fds[0].revents = 0;
        nfds_t poll_count = 1;
        if (wake_read_fd_ >= 0) {
            fds[1].fd = wake_read_fd_;
            fds[1].events = POLLIN;
            fds[1].revents = 0;
            poll_count = 2;
        }

        int ret = poll(fds, poll_count, kClientIdleTimeoutMs);
        if (ret < 0) {
            if (errno == EINTR) continue;
            return;
        }
        if (ret == 0) {
            return;
        }
        if (poll_count > 1 && (fds[1].revents & (POLLIN | POLLERR | POLLHUP | POLLNVAL))) {
            return;
        }
        if (fds[0].revents & (POLLERR | POLLNVAL)) {
            return;
        }
        if (!(fds[0].revents & (POLLIN | POLLHUP))) {
            continue;
        }

        ssize_t n = recv(client_fd, buffer, sizeof(buffer), 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return;
        }
        if (n == 0) break;
        request.append(buffer, static_cast<size_t>(n));
        if (request.size() > kMaxRequestBytes) {
            return;
        }
        if (request.find('\n') != std::string::npos) {
            break;
        }
    }
    std::string response = handle_request(request);
    (void)send_all(client_fd, response);
}

} // namespace network_service
