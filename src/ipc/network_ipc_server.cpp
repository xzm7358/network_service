#include "ipc/network_ipc_server.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <poll.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include "ipc/network_ipc_v1_business_dispatch.h"
#include "ipc/network_ipc_v1_codec.h"
#include "ipc/network_ipc_v1_outbound.h"
#include "ipc/network_ipc_v1_rebase.h"
#include "ipc/network_ipc_v1_session.h"
#include "network_service_protocol.h"
#include "service/network_daemon.h"
#include "service/network_state_change_detector.h"

namespace network_service {

namespace {

using Clock = std::chrono::steady_clock;

static int g_wake_fd = -1;
constexpr int kClientHandshakeIdleTimeoutMs = 1000;
constexpr int kStateObservationIntervalMs = 250;
constexpr size_t kMaxRequestBytes = 64 * 1024;
constexpr std::size_t kMaxActiveClients = 8;
constexpr std::size_t kReadBudgetPerTick = 16 * 1024;
constexpr std::size_t kWriteBudgetPerTick = 64 * 1024;
constexpr int kFramesPerClientTick = 4;
constexpr std::uint8_t kV1Magic[4] = {'N', 'S', 'P', '1'};

enum class ClientProtocol {
    Undecided = 0,
    V0,
    V1,
};

struct ClientState {
    explicit ClientState(int accepted_fd, Clock::time_point now)
        : fd(accepted_fd),
          last_input_activity(now),
          write_progress_at(now) {}

    int fd = -1;
    ClientProtocol protocol = ClientProtocol::Undecided;
    std::vector<std::uint8_t> initial;
    std::string v0_request;
    std::unique_ptr<ipc_v1::Session> session;
    ipc_v1::FrameDecoder decoder;
    ipc_v1::OutboundQueue outbound;
    Clock::time_point last_input_activity;
    Clock::time_point write_progress_at;
    bool write_timer_active = false;
    bool close_after_flush = false;
    bool closed = false;
};

static std::uint64_t make_server_generation() {
    const auto now = std::chrono::system_clock::now().time_since_epoch().count();
    std::uint64_t generation = static_cast<std::uint64_t>(now);
    generation ^= static_cast<std::uint64_t>(static_cast<unsigned long>(getpid())) << 32;
    return generation == 0 ? 1 : generation;
}

static bool set_nonblocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
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

static int send_flags() {
    int flags = 0;
#ifdef MSG_NOSIGNAL
    flags |= MSG_NOSIGNAL;
#endif
#ifdef MSG_DONTWAIT
    flags |= MSG_DONTWAIT;
#endif
    return flags;
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
        (void)set_nonblocking(wake_read_fd_);
        (void)set_nonblocking(wake_write_fd_);
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
    if (!set_nonblocking(listen_fd_)) {
        std::cerr << "network_service: failed to make listen socket non-blocking: "
                  << strerror(errno) << std::endl;
        close(listen_fd_);
        listen_fd_ = -1;
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
        std::cerr << "network_service: bind " << socket_path_ << " failed: "
                  << strerror(errno) << std::endl;
        close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    chmod(socket_path_.c_str(), 0666);

    if (::listen(listen_fd_, static_cast<int>(kMaxActiveClients)) != 0) {
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
    std::vector<std::unique_ptr<ClientState>> clients;
    NetworkStateChangeDetector state_detector;
    (void)state_detector.observe(daemon_.snapshot());
    Clock::time_point next_state_observation =
        Clock::now() + std::chrono::milliseconds(kStateObservationIntervalMs);

    auto close_client = [](ClientState &client) {
        if (client.fd >= 0) {
            shutdown(client.fd, SHUT_RDWR);
            close(client.fd);
            client.fd = -1;
        }
        client.closed = true;
    };

    auto cleanup_clients = [&]() {
        clients.erase(
            std::remove_if(
                clients.begin(),
                clients.end(),
                [&](std::unique_ptr<ClientState> &client) {
                    if (!client->closed) return false;
                    close_client(*client);
                    return true;
                }),
            clients.end());
    };

    auto enqueue_frame = [&](ClientState &client,
                             std::vector<std::uint8_t> frame) -> bool {
        const bool was_empty = client.outbound.empty();
        const ipc_v1::OutboundEnqueueResult result =
            client.outbound.enqueue(std::move(frame));
        if (result == ipc_v1::OutboundEnqueueResult::Accepted) {
            if (was_empty) {
                client.write_timer_active = true;
                client.write_progress_at = Clock::now();
            }
            return true;
        }

        std::cerr << "network_service: IPC_V1_OUTBOUND_OVERFLOW result="
                  << outbound_enqueue_name(result)
                  << " queued_frames=" << client.outbound.frame_count()
                  << " queued_bytes=" << client.outbound.queued_bytes()
                  << " max_frames=" << client.outbound.max_frames()
                  << " max_bytes=" << client.outbound.max_bytes() << std::endl;
        client.closed = true;
        return false;
    };

    auto broadcast_event = [&](const std::string &event_name,
                               const std::string &payload_json) -> bool {
        const std::vector<std::uint8_t> event =
            event_sequencer_.encode_event(event_name, payload_json);
        if (event.empty()) {
            std::cerr << "network_service: IPC_V1_EVENT_ENCODE_FAILED event="
                      << event_name << std::endl;
            return false;
        }

        for (auto &entry : clients) {
            ClientState &recipient = *entry;
            if (recipient.closed || recipient.protocol != ClientProtocol::V1 ||
                !recipient.session || !recipient.session->ready() ||
                !recipient.session->events_subscribed()) {
                continue;
            }
            std::vector<std::uint8_t> copy = event;
            (void)enqueue_frame(recipient, std::move(copy));
        }
        return true;
    };

    auto observe_state_if_due = [&]() {
        const Clock::time_point now = Clock::now();
        if (now < next_state_observation) return;
        next_state_observation =
            now + std::chrono::milliseconds(kStateObservationIntervalMs);

        const NetworkStateChangeSet changes = state_detector.observe(daemon_.snapshot());
        if (!changes.any()) return;
        if (!broadcast_event("network.state.changed", changes.payload_json())) {
            std::cerr << "network_service: IPC_V1_STATE_EVENT_ALLOCATION_FAILED"
                      << std::endl;
        }
    };

    auto flush_client = [&](ClientState &client) {
        if (client.closed || client.outbound.empty()) {
            if (client.outbound.empty()) client.write_timer_active = false;
            if (client.close_after_flush && client.outbound.empty()) {
                client.closed = true;
            }
            return;
        }

        std::size_t budget = kWriteBudgetPerTick;
        while (!client.outbound.empty() && budget > 0) {
            const std::size_t attempt = std::min(client.outbound.front_size(), budget);
            const ssize_t written =
                send(client.fd, client.outbound.front_data(), attempt, send_flags());
            if (written > 0) {
                const std::size_t consumed = static_cast<std::size_t>(written);
                if (!client.outbound.consume(consumed)) {
                    client.closed = true;
                    return;
                }
                budget -= consumed;
                client.write_progress_at = Clock::now();
                client.write_timer_active = !client.outbound.empty();
                continue;
            }
            if (written == 0) {
                client.closed = true;
                return;
            }
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (!client.write_timer_active) {
                    client.write_timer_active = true;
                    client.write_progress_at = Clock::now();
                }
                return;
            }
            std::cerr << "network_service: IPC_V1_OUTBOUND_WRITE_FAILED errno="
                      << errno << " message=" << strerror(errno) << std::endl;
            client.closed = true;
            return;
        }

        if (client.outbound.empty()) {
            client.write_timer_active = false;
            if (client.close_after_flush) client.closed = true;
        } else if (!client.write_timer_active) {
            client.write_timer_active = true;
            client.write_progress_at = Clock::now();
        }
    };

    auto process_v1_frame = [&](ClientState &client) -> bool {
        if (!client.session || !client.decoder.has_frame() || client.closed) return false;

        const ipc_v1::Frame frame = client.decoder.take_frame();
        ipc_v1::Session::HandleResult result = client.session->handle_frame(frame);
        if (!result.response.empty() &&
            !enqueue_frame(client, std::move(result.response))) {
            return false;
        }

        if (result.server_action == ipc_v1::Session::ServerAction::EmitEventsSubscribed) {
            if (!broadcast_event("network.events.subscribed", "{}")) {
                client.closed = true;
                return false;
            }
        } else if (result.server_action ==
                   ipc_v1::Session::ServerAction::SendAuthoritativeSnapshot) {
            std::vector<std::uint8_t> response = ipc_v1::encode_snapshot_response(
                result.action_request_id,
                generation_,
                event_sequencer_.last_sequence(),
                daemon_.snapshot_result_json());
            if (response.empty() || !enqueue_frame(client, std::move(response))) {
                client.closed = true;
                return false;
            }
        } else if (result.server_action ==
                   ipc_v1::Session::ServerAction::DispatchBusinessRequest) {
            std::vector<std::uint8_t> response = ipc_v1::dispatch_business_request(
                daemon_,
                result.action_request_id,
                result.action_method,
                result.action_params_json);
            if (response.empty() || !enqueue_frame(client, std::move(response))) {
                client.closed = true;
                return false;
            }
        }

        if (result.close_after_send) client.close_after_flush = true;
        return !client.closed;
    };

    auto service_buffered_v1 = [&](ClientState &client) {
        if (client.protocol != ClientProtocol::V1 || !client.session || client.closed) return;

        for (int i = 0; i < kFramesPerClientTick; ++i) {
            if (client.closed || client.close_after_flush ||
                !client.outbound.empty() || !client.decoder.has_frame()) {
                break;
            }
            if (!process_v1_frame(client)) break;
            if (!client.outbound.empty()) {
                flush_client(client);
            }
        }
    };

    auto complete_v0_if_ready = [&](ClientState &client) {
        if (client.protocol != ClientProtocol::V0 || client.closed ||
            client.close_after_flush) {
            return;
        }
        if (client.v0_request.find('\n') == std::string::npos) return;

        const std::string response = handle_request(client.v0_request);
        std::vector<std::uint8_t> bytes(response.begin(), response.end());
        if (enqueue_frame(client, std::move(bytes))) {
            client.close_after_flush = true;
            flush_client(client);
        }
    };

    auto commit_protocol_if_possible = [&](ClientState &client) {
        if (client.protocol != ClientProtocol::Undecided ||
            client.initial.size() < sizeof(kV1Magic) || client.closed) {
            return;
        }

        if (starts_with_v1_magic(client.initial)) {
            client.protocol = ClientProtocol::V1;
            std::ostringstream session_id;
            session_id << "ns-" << generation_ << '-' << next_session_sequence_++;
            client.session =
                std::make_unique<ipc_v1::Session>(generation_, session_id.str());

            if (client.decoder.feed(client.initial) == ipc_v1::DecodeStatus::Error) {
                client.closed = true;
            }
            client.initial.clear();
            service_buffered_v1(client);
            return;
        }

        client.protocol = ClientProtocol::V0;
        client.v0_request.assign(client.initial.begin(), client.initial.end());
        client.initial.clear();
        if (client.v0_request.size() > kMaxRequestBytes) {
            client.closed = true;
            return;
        }
        complete_v0_if_ready(client);
    };

    auto receive_client = [&](ClientState &client) {
        if (client.closed || !client.outbound.empty() || client.close_after_flush) return;

        char buffer[4096];
        std::size_t budget = kReadBudgetPerTick;
        while (budget > 0 && !client.closed && client.outbound.empty() &&
               !client.close_after_flush) {
            const std::size_t attempt = std::min<std::size_t>(sizeof(buffer), budget);
            const ssize_t n = recv(client.fd, buffer, attempt, MSG_DONTWAIT);
            if (n > 0) {
                const std::size_t count = static_cast<std::size_t>(n);
                budget -= count;
                client.last_input_activity = Clock::now();

                if (client.protocol == ClientProtocol::Undecided) {
                    const auto *bytes =
                        reinterpret_cast<const std::uint8_t *>(buffer);
                    client.initial.insert(client.initial.end(), bytes, bytes + count);
                    if (client.initial.size() > kMaxRequestBytes) {
                        client.closed = true;
                        break;
                    }
                    commit_protocol_if_possible(client);
                } else if (client.protocol == ClientProtocol::V0) {
                    client.v0_request.append(buffer, count);
                    if (client.v0_request.size() > kMaxRequestBytes) {
                        client.closed = true;
                        break;
                    }
                    complete_v0_if_ready(client);
                } else {
                    const auto *bytes =
                        reinterpret_cast<const std::uint8_t *>(buffer);
                    if (client.decoder.feed(bytes, count) ==
                        ipc_v1::DecodeStatus::Error) {
                        client.closed = true;
                        break;
                    }
                    service_buffered_v1(client);
                }

                if (client.protocol == ClientProtocol::V1 &&
                    client.decoder.has_frame()) {
                    service_buffered_v1(client);
                }
                continue;
            }

            if (n == 0) {
                client.closed = true;
                break;
            }
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            client.closed = true;
            break;
        }
    };

    auto needs_input_deadline = [](const ClientState &client) {
        if (client.close_after_flush) return false;
        if (client.protocol == ClientProtocol::Undecided ||
            client.protocol == ClientProtocol::V0) {
            return true;
        }
        return !client.session || !client.session->ready();
    };

    auto apply_deadlines = [&]() {
        const Clock::time_point now = Clock::now();
        for (auto &entry : clients) {
            ClientState &client = *entry;
            if (client.closed) continue;

            if (needs_input_deadline(client) &&
                now - client.last_input_activity >=
                    std::chrono::milliseconds(kClientHandshakeIdleTimeoutMs)) {
                client.closed = true;
                continue;
            }

            if (!client.outbound.empty() && client.write_timer_active &&
                now - client.write_progress_at >=
                    std::chrono::milliseconds(ipc_v1::kDefaultWriteStallTimeoutMs)) {
                std::cerr << "network_service: IPC_V1_OUTBOUND_WRITE_STALLED timeout_ms="
                          << ipc_v1::kDefaultWriteStallTimeoutMs << std::endl;
                client.closed = true;
            }
        }
    };

    auto poll_timeout_ms = [&]() {
        const Clock::time_point now = Clock::now();
        int timeout = -1;

        auto include_deadline = [&](Clock::time_point deadline) {
            const auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
                    .count();
            int candidate = 0;
            if (remaining > 0) {
                candidate =
                    remaining > INT_MAX ? INT_MAX : static_cast<int>(remaining);
            }
            if (timeout < 0 || candidate < timeout) timeout = candidate;
        };

        for (const auto &entry : clients) {
            const ClientState &client = *entry;
            if (client.closed) continue;

            if (client.protocol == ClientProtocol::V1 && client.session &&
                client.decoder.has_frame() && client.outbound.empty() &&
                !client.close_after_flush) {
                return 0;
            }

            if (needs_input_deadline(client)) {
                include_deadline(
                    client.last_input_activity +
                    std::chrono::milliseconds(kClientHandshakeIdleTimeoutMs));
            }
            if (!client.outbound.empty() && client.write_timer_active) {
                include_deadline(
                    client.write_progress_at +
                    std::chrono::milliseconds(ipc_v1::kDefaultWriteStallTimeoutMs));
            }
        }
        include_deadline(next_state_observation);
        return timeout;
    };

    while (running_) {
        observe_state_if_due();
        for (auto &entry : clients) {
            service_buffered_v1(*entry);
            if (!entry->outbound.empty()) flush_client(*entry);
        }

        apply_deadlines();
        cleanup_clients();
        if (!running_) break;

        std::vector<struct pollfd> fds;
        std::vector<ClientState *> polled_clients;
        fds.reserve(2 + clients.size());
        polled_clients.reserve(clients.size());

        struct pollfd listen_poll;
        listen_poll.fd = listen_fd_;
        listen_poll.events = POLLIN;
        listen_poll.revents = 0;
        fds.push_back(listen_poll);

        std::size_t wake_index = static_cast<std::size_t>(-1);
        if (wake_read_fd_ >= 0) {
            wake_index = fds.size();
            struct pollfd wake_poll;
            wake_poll.fd = wake_read_fd_;
            wake_poll.events = POLLIN;
            wake_poll.revents = 0;
            fds.push_back(wake_poll);
        }

        for (auto &entry : clients) {
            ClientState &client = *entry;
            struct pollfd client_poll;
            client_poll.fd = client.fd;
            client_poll.events = client.outbound.empty() && !client.close_after_flush
                                     ? POLLIN
                                     : POLLOUT;
            client_poll.revents = 0;
            fds.push_back(client_poll);
            polled_clients.push_back(&client);
        }

        const int ret =
            poll(fds.data(), static_cast<nfds_t>(fds.size()), poll_timeout_ms());
        if (ret < 0) {
            if (errno == EINTR) continue;
            std::cerr << "network_service: poll failed: " << strerror(errno)
                      << std::endl;
            continue;
        }
        if (ret == 0) continue;

        if (wake_index != static_cast<std::size_t>(-1) &&
            (fds[wake_index].revents &
             (POLLIN | POLLERR | POLLHUP | POLLNVAL))) {
            char drain[32];
            while (read(wake_read_fd_, drain, sizeof(drain)) > 0) {}
            running_ = false;
            break;
        }

        if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            running_ = false;
            break;
        }

        if (fds[0].revents & POLLIN) {
            while (true) {
                int client_fd = accept(listen_fd_, nullptr, nullptr);
                if (client_fd < 0) {
                    if (errno == EINTR) continue;
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    if (!running_ || listen_fd_ < 0) break;
                    std::cerr << "network_service: accept failed: "
                              << strerror(errno) << std::endl;
                    break;
                }

                if (!set_nonblocking(client_fd)) {
                    close(client_fd);
                    continue;
                }

                if (clients.size() >= kMaxActiveClients) {
                    std::cerr
                        << "network_service: IPC_CLIENT_CAPACITY_REJECTED max_clients="
                        << kMaxActiveClients << std::endl;
                    shutdown(client_fd, SHUT_RDWR);
                    close(client_fd);
                    continue;
                }

                clients.push_back(
                    std::make_unique<ClientState>(client_fd, Clock::now()));
            }
        }

        const std::size_t client_poll_offset =
            wake_index == static_cast<std::size_t>(-1) ? 1 : 2;
        for (std::size_t i = 0; i < polled_clients.size(); ++i) {
            ClientState &client = *polled_clients[i];
            if (client.closed) continue;

            const short revents = fds[client_poll_offset + i].revents;
            if (revents == 0) continue;
            if (revents & (POLLERR | POLLNVAL)) {
                client.closed = true;
                continue;
            }

            if ((revents & POLLOUT) && !client.outbound.empty()) {
                flush_client(client);
            }
            if (client.closed) continue;

            if ((revents & (POLLIN | POLLHUP)) && client.outbound.empty() &&
                !client.close_after_flush) {
                receive_client(client);
            } else if ((revents & POLLHUP) && client.outbound.empty()) {
                client.closed = true;
            }

            if (!client.closed) {
                service_buffered_v1(client);
                if (!client.outbound.empty()) flush_client(client);
            }
        }
    }

    for (auto &entry : clients) {
        close_client(*entry);
    }
    clients.clear();
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
                                           extract_json_string(request, "dns")) +
               "\n";
    }
    if (method == "wpa.events") {
        return daemon_.wpa_events_json() + "\n";
    }
    if (method == "wifi.scan") {
        return daemon_.wifi_scan_json() + "\n";
    }
    if (method == "wifi.set_enabled") {
        std::string enabled = extract_json_string(request, "enabled");
        return daemon_.wifi_set_enabled_json(enabled == "1" || enabled == "true") +
               "\n";
    }
    if (method == "wifi.connect") {
        return daemon_.wifi_connect_json(extract_json_string(request, "ssid"),
                                         extract_json_string(request, "password")) +
               "\n";
    }
    if (method == "wifi.connect_saved") {
        return daemon_.wifi_connect_saved_json(
                   extract_json_string(request, "ssid")) +
               "\n";
    }
    if (method == "wifi.saved_list") {
        return daemon_.wifi_list_saved_json() + "\n";
    }
    if (method == "wifi.forget") {
        return daemon_.wifi_forget_json(extract_json_string(request, "ssid")) +
               "\n";
    }
    if (method == "wifi.autoconnect") {
        std::string enabled = extract_json_string(request, "enabled");
        return daemon_.wifi_set_autoconnect_json(
                   extract_json_string(request, "ssid"),
                   enabled == "1" || enabled == "true") +
               "\n";
    }
    if (method == "wifi.disconnect") {
        return daemon_.wifi_disconnect_json() + "\n";
    }
    return "{\"status\":404,\"error\":\"unknown method\"}\n";
}

} // namespace network_service
