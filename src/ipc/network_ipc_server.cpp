#include "ipc/network_ipc_server.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>

#include "ipc/network_ipc_v1_codec.h"
#include "ipc/network_ipc_v1_outbound.h"
#include "ipc/network_ipc_v1_rebase.h"
#include "ipc/network_ipc_v1_session.h"
#include "network_service_protocol.h"
#include "service/network_daemon.h"

namespace network_service {

namespace {

static int g_wake_fd = -1;
constexpr int kClientIdleTimeoutMs = 1000;
constexpr size_t kMaxRequestBytes = 64 * 1024;
constexpr std::uint8_t kV1Magic[4] = {'N', 'S', 'P', '1'};

static std::uint64_t make_server_generation() {
    const auto now = std::chrono::system_clock::now().time_since_epoch().count();
    std::uint64_t generation = static_cast<std::uint64_t>(now);
    generation ^= static_cast<std::uint64_t>(static_cast<unsigned long>(getpid())) << 32;
    return generation == 0 ? 1 : generation;
}

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

static bool send_all_bytes(int fd, const std::uint8_t *data, size_t size) {
    const std::uint8_t *p = data;
    size_t remaining = size;
    while (remaining > 0) {
#ifdef MSG_NOSIGNAL
        const ssize_t written = send(fd, p, remaining, MSG_NOSIGNAL);
#else
        const ssize_t written = send(fd, p, remaining, 0);
#endif
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

static bool send_all(int fd, const std::string &data) {
    return send_all_bytes(fd,
                          reinterpret_cast<const std::uint8_t *>(data.data()),
                          data.size());
}

static bool starts_with_v1_magic(const std::vector<std::uint8_t> &data) {
    return data.size() >= sizeof(kV1Magic) &&
           std::equal(kV1Magic, kV1Magic + sizeof(kV1Magic), data.begin());
}

static const char *outbound_enqueue_name(ipc_v1::OutboundEnqueueResult result) {
    switch (result) {
    case ipc_v1::OutboundEnqueueResult::Accepted: return "accepted";
    case ipc_v1::OutboundEnqueueResult::InvalidFrame: return "invalid_frame";
    case ipc_v1::OutboundEnqueueResult::FrameTooLarge: return "frame_too_large";
    case ipc_v1::OutboundEnqueueResult::Overflow: return "overflow";
    }
    return "unknown";
}

static const char *outbound_flush_name(ipc_v1::OutboundFlushResult result) {
    switch (result) {
    case ipc_v1::OutboundFlushResult::Drained: return "drained";
    case ipc_v1::OutboundFlushResult::SlowClient: return "slow_client";
    case ipc_v1::OutboundFlushResult::Interrupted: return "interrupted";
    case ipc_v1::OutboundFlushResult::Disconnected: return "disconnected";
    }
    return "unknown";
}

} // namespace

NetworkIpcServer::NetworkIpcServer(NetworkDaemon &daemon)
    : daemon_(daemon),
      generation_(make_server_generation()),
      event_sequencer_(generation_) {
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
    std::vector<std::uint8_t> initial;
    char buffer[1024];

    while (initial.size() < sizeof(kV1Magic)) {
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

        const int ret = poll(fds, poll_count, kClientIdleTimeoutMs);
        if (ret < 0) {
            if (errno == EINTR) continue;
            return;
        }
        if (ret == 0) return;
        if (poll_count > 1 && (fds[1].revents & (POLLIN | POLLERR | POLLHUP | POLLNVAL))) return;
        if (fds[0].revents & (POLLERR | POLLNVAL)) return;
        if (!(fds[0].revents & (POLLIN | POLLHUP))) continue;

        const ssize_t n = recv(client_fd, buffer, sizeof(buffer), 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return;
        }
        if (n == 0) break;
        initial.insert(initial.end(),
                       reinterpret_cast<const std::uint8_t *>(buffer),
                       reinterpret_cast<const std::uint8_t *>(buffer) + n);
        if (initial.size() > kMaxRequestBytes) return;
        if (std::find(initial.begin(), initial.end(), static_cast<std::uint8_t>('\n')) != initial.end()) break;
    }

    if (starts_with_v1_magic(initial)) {
        handle_v1_client(client_fd, initial);
    } else {
        handle_v0_client(client_fd, initial);
    }
}

void NetworkIpcServer::handle_v0_client(int client_fd, const std::vector<std::uint8_t> &initial) {
    std::string request(initial.begin(), initial.end());
    char buffer[1024];

    while (request.find('\n') == std::string::npos) {
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

        const int ret = poll(fds, poll_count, kClientIdleTimeoutMs);
        if (ret < 0) {
            if (errno == EINTR) continue;
            return;
        }
        if (ret == 0) return;
        if (poll_count > 1 && (fds[1].revents & (POLLIN | POLLERR | POLLHUP | POLLNVAL))) return;
        if (fds[0].revents & (POLLERR | POLLNVAL)) return;
        if (!(fds[0].revents & (POLLIN | POLLHUP))) continue;

        const ssize_t n = recv(client_fd, buffer, sizeof(buffer), 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return;
        }
        if (n == 0) break;
        request.append(buffer, static_cast<size_t>(n));
        if (request.size() > kMaxRequestBytes) return;
    }

    const std::string response = handle_request(request);
    (void)send_all(client_fd, response);
}

void NetworkIpcServer::handle_v1_client(int client_fd, const std::vector<std::uint8_t> &initial) {
    std::ostringstream session_id;
    session_id << "ns-" << generation_ << '-' << next_session_sequence_++;
    ipc_v1::Session session(generation_, session_id.str());
    ipc_v1::FrameDecoder decoder;
    ipc_v1::OutboundWriter outbound;

    auto enqueue_frame = [&](std::vector<std::uint8_t> frame) -> bool {
        const ipc_v1::OutboundEnqueueResult result = outbound.enqueue(std::move(frame));
        if (result == ipc_v1::OutboundEnqueueResult::Accepted) return true;
        std::cerr << "network_service: IPC_V1_OUTBOUND_OVERFLOW result="
                  << outbound_enqueue_name(result)
                  << " queued_frames=" << outbound.queue().frame_count()
                  << " queued_bytes=" << outbound.queue().queued_bytes()
                  << " max_frames=" << outbound.queue().max_frames()
                  << " max_bytes=" << outbound.queue().max_bytes() << std::endl;
        return false;
    };

    auto flush_outbound = [&]() -> bool {
        const ipc_v1::OutboundFlushResult result = outbound.flush(client_fd, wake_read_fd_);
        if (result == ipc_v1::OutboundFlushResult::Drained) return true;
        if (result == ipc_v1::OutboundFlushResult::SlowClient) {
            std::cerr << "network_service: IPC_V1_OUTBOUND_WRITE_STALLED timeout_ms="
                      << ipc_v1::kDefaultWriteStallTimeoutMs << std::endl;
        } else if (result != ipc_v1::OutboundFlushResult::Interrupted) {
            std::cerr << "network_service: IPC_V1_OUTBOUND_WRITE_FAILED result="
                      << outbound_flush_name(result) << std::endl;
        }
        return false;
    };

    auto process_frames = [&]() -> bool {
        bool close_after_flush = false;
        while (decoder.has_frame()) {
            const ipc_v1::Frame frame = decoder.take_frame();
            ipc_v1::Session::HandleResult result = session.handle_frame(frame);
            if (!result.response.empty() && !enqueue_frame(std::move(result.response))) return false;
            if (result.server_action == ipc_v1::Session::ServerAction::EmitEventsSubscribed) {
                std::vector<std::uint8_t> event =
                    event_sequencer_.encode_event("network.events.subscribed", "{}");
                if (event.empty() || !enqueue_frame(std::move(event))) return false;
            } else if (result.server_action ==
                       ipc_v1::Session::ServerAction::SendAuthoritativeSnapshot) {
                std::vector<std::uint8_t> response = ipc_v1::encode_snapshot_response(
                    result.action_request_id,
                    generation_,
                    event_sequencer_.last_sequence(),
                    daemon_.snapshot_result_json());
                if (response.empty() || !enqueue_frame(std::move(response))) return false;
            }
            if (result.close_after_send) {
                close_after_flush = true;
                break;
            }
        }
        if (!flush_outbound()) return false;
        return !close_after_flush;
    };

    if (decoder.feed(initial) == ipc_v1::DecodeStatus::Error) return;
    if (!process_frames()) return;

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

        const int ret = poll(fds, poll_count, kClientIdleTimeoutMs);
        if (ret < 0) {
            if (errno == EINTR) continue;
            return;
        }
        if (ret == 0) return;
        if (poll_count > 1 && (fds[1].revents & (POLLIN | POLLERR | POLLHUP | POLLNVAL))) return;
        if (fds[0].revents & (POLLERR | POLLNVAL)) return;
        if (!(fds[0].revents & (POLLIN | POLLHUP))) continue;

        const ssize_t n = recv(client_fd, buffer, sizeof(buffer), 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return;
        }
        if (n == 0) return;

        const auto *bytes = reinterpret_cast<const std::uint8_t *>(buffer);
        if (decoder.feed(bytes, static_cast<size_t>(n)) == ipc_v1::DecodeStatus::Error) return;
        if (!process_frames()) return;
    }
}

} // namespace network_service
