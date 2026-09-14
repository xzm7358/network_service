#include "network_service/v1_client.h"

#include "ipc/network_ipc_v1_codec.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>

namespace network_service {
namespace {

using Clock = std::chrono::steady_clock;
using ipc_v1::CodecError;
using ipc_v1::Frame;
using ipc_v1::MessageType;

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               Clock::now().time_since_epoch())
        .count();
}

bool wait_fd(int fd, short events, std::int64_t deadline) {
    for (;;) {
        const std::int64_t remaining = deadline - now_ms();
        if (remaining <= 0) return false;
        pollfd descriptor{};
        descriptor.fd = fd;
        descriptor.events = events;
        const int rc = ::poll(&descriptor, 1,
                              static_cast<int>(std::min<std::int64_t>(remaining, INT_MAX)));
        if (rc > 0) {
            if (descriptor.revents & events) return true;
            if (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) return false;
            continue;
        }
        if (rc == 0) return false;
        if (errno != EINTR) return false;
    }
}

bool write_all(int fd, const std::uint8_t *bytes, std::size_t size,
               std::int64_t deadline, std::string &error) {
    std::size_t offset = 0;
    while (offset < size) {
        if (!wait_fd(fd, POLLOUT, deadline)) {
            error = "timed out writing IPC frame";
            return false;
        }
#ifdef MSG_NOSIGNAL
        const ssize_t count = ::send(fd, bytes + offset, size - offset, MSG_NOSIGNAL);
#else
        const ssize_t count = ::send(fd, bytes + offset, size - offset, 0);
#endif
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
            continue;
        }
        error = count == 0 ? "socket closed while writing IPC frame"
                           : std::string("IPC send failed: ") + std::strerror(errno);
        return false;
    }
    return true;
}

bool json_u64(const std::string &json, const char *key, std::uint64_t &out) {
    const std::string needle = std::string("\"") + key + "\":";
    const std::size_t found = json.find(needle);
    if (found == std::string::npos) return false;
    std::size_t pos = found + needle.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
    if (pos == json.size() || json[pos] < '0' || json[pos] > '9') return false;
    std::uint64_t value = 0;
    while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
        const unsigned digit = static_cast<unsigned>(json[pos] - '0');
        if (value > (UINT64_MAX - digit) / 10U) return false;
        value = value * 10U + digit;
        ++pos;
    }
    out = value;
    return true;
}

bool contains_json_string(const std::string &json, const char *key, const char *value) {
    return json.find(std::string("\"") + key + "\":\"" + value + "\"") !=
           std::string::npos;
}

} // namespace

class NetworkServiceClient::Impl {
public:
    bool send(MessageType type, const std::string &payload, int timeout_ms,
              std::string &error) {
        CodecError codec_error = CodecError::None;
        const auto bytes = ipc_v1::encode_frame(type, payload, codec_error);
        if (codec_error != CodecError::None || bytes.empty()) {
            error = std::string("failed to encode IPC frame: ") +
                    ipc_v1::codec_error_string(codec_error);
            return false;
        }
        return write_all(fd, bytes.data(), bytes.size(), now_ms() + timeout_ms, error);
    }

    bool drain(std::string &error) {
        std::uint8_t bytes[4096];
        for (;;) {
            const ssize_t count = ::recv(fd, bytes, sizeof(bytes), MSG_DONTWAIT);
            if (count > 0) {
                if (decoder.feed(bytes, static_cast<std::size_t>(count)) ==
                    ipc_v1::DecodeStatus::Error) {
                    error = std::string("invalid IPC frame: ") +
                            ipc_v1::codec_error_string(decoder.error());
                    return false;
                }
                continue;
            }
            if (count == 0) {
                error = "network service closed the IPC session";
                return false;
            }
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
            error = std::string("IPC receive failed: ") + std::strerror(errno);
            return false;
        }
    }

    bool wait_frame(Frame &frame, int timeout_ms, std::string &error) {
        const std::int64_t deadline = now_ms() + timeout_ms;
        for (;;) {
            if (decoder.has_frame()) {
                frame = decoder.take_frame();
                return true;
            }
            if (!wait_fd(fd, POLLIN, deadline)) {
                error = "timed out waiting for IPC frame";
                return false;
            }
            if (!drain(error)) return false;
        }
    }

    int fd{-1};
    std::uint64_t next_request_id{1};
    std::uint64_t generation{0};
    std::uint64_t snapshot_sequence{0};
    bool baseline_valid{false};
    bool rebase_required{true};
    ipc_v1::FrameDecoder decoder;
    std::deque<Frame> pending_events;
};

NetworkServiceClient::NetworkServiceClient() : impl_(new Impl) {}
NetworkServiceClient::~NetworkServiceClient() { close(); }

bool NetworkServiceClient::connectTo(const std::string &socket_path,
                                     std::string &error,
                                     int timeout_ms) {
    close();
    error.clear();
    sockaddr_un address{};
    if (socket_path.empty() || socket_path.size() >= sizeof(address.sun_path)) {
        error = "invalid network service socket path";
        return false;
    }
    impl_->fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (impl_->fd < 0) {
        error = std::string("IPC socket failed: ") + std::strerror(errno);
        return false;
    }
    const int flags = ::fcntl(impl_->fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(impl_->fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        error = std::string("failed to make IPC socket non-blocking: ") + std::strerror(errno);
        close();
        return false;
    }
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, socket_path.c_str(), socket_path.size() + 1U);
    if (::connect(impl_->fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
        if (errno != EINPROGRESS || !wait_fd(impl_->fd, POLLOUT, now_ms() + timeout_ms)) {
            error = std::string("IPC connect failed: ") + std::strerror(errno);
            close();
            return false;
        }
        int socket_error = 0;
        socklen_t length = sizeof(socket_error);
        if (::getsockopt(impl_->fd, SOL_SOCKET, SO_ERROR, &socket_error, &length) != 0 ||
            socket_error != 0) {
            error = std::string("IPC connect failed: ") + std::strerror(socket_error);
            close();
            return false;
        }
    }

    const std::string hello =
        R"({"minVersion":1,"maxVersion":1,"client":"smartcontrol","capabilities":["events","snapshot-rebase"]})";
    if (!impl_->send(MessageType::Hello, hello, timeout_ms, error)) {
        close();
        return false;
    }
    Frame ready;
    if (!impl_->wait_frame(ready, timeout_ms, error) ||
        ready.header.type != MessageType::Ready ||
        !contains_json_string(ready.payload, "service", "network_service") ||
        !json_u64(ready.payload, "generation", impl_->generation) ||
        impl_->generation == 0) {
        if (error.empty()) error = "HELLO did not produce a valid READY frame";
        close();
        return false;
    }
    impl_->rebase_required = true;
    return true;
}

void NetworkServiceClient::close() {
    if (impl_->fd >= 0) ::close(impl_->fd);
    impl_->fd = -1;
    impl_->next_request_id = 1;
    impl_->generation = 0;
    impl_->snapshot_sequence = 0;
    impl_->baseline_valid = false;
    impl_->rebase_required = true;
    impl_->decoder.reset();
    impl_->pending_events.clear();
}

int NetworkServiceClient::fd() const { return impl_->fd; }

bool NetworkServiceClient::request(const std::string &method,
                                   const std::string &params_json,
                                   std::string &response_json,
                                   std::string &error,
                                   int timeout_ms) {
    error.clear();
    if (impl_->fd < 0) {
        error = "client is not connected";
        return false;
    }
    const std::uint64_t id = impl_->next_request_id++;
    const std::string payload = "{\"requestId\":" + std::to_string(id) +
                                ",\"method\":" + jsonQuote(method) +
                                ",\"params\":" + params_json + "}";
    if (!impl_->send(MessageType::Request, payload, timeout_ms, error)) return false;

    for (;;) {
        Frame frame;
        if (!impl_->wait_frame(frame, timeout_ms, error)) return false;
        if (frame.header.type == MessageType::Event) {
            impl_->pending_events.push_back(std::move(frame));
            continue;
        }
        if (frame.header.type == MessageType::Error) {
            error = "network service session error: " + frame.payload;
            return false;
        }
        if (frame.header.type != MessageType::Response) {
            error = "unexpected IPC frame while waiting for response";
            return false;
        }
        std::uint64_t response_id = 0;
        if (!json_u64(frame.payload, "requestId", response_id) || response_id != id) {
            error = "IPC requestId correlation mismatch";
            return false;
        }
        response_json = std::move(frame.payload);
        if (method == "network.snapshot") {
            std::uint64_t response_generation = 0;
            std::uint64_t snapshot_sequence = 0;
            if (!json_u64(response_json, "generation", response_generation) ||
                !json_u64(response_json, "snapshotSeq", snapshot_sequence) ||
                response_generation != impl_->generation) {
                error = "network.snapshot is missing a valid rebase point";
                return false;
            }
            impl_->snapshot_sequence = snapshot_sequence;
            impl_->baseline_valid = true;
            impl_->rebase_required = false;
        }
        return true;
    }
}

bool NetworkServiceClient::readNext(std::string &event_json, std::string &error) {
    error.clear();
    if (impl_->fd < 0) {
        error = "client is not connected";
        return false;
    }
    if (!impl_->drain(error)) return false;
    while (!impl_->pending_events.empty() || impl_->decoder.has_frame()) {
        Frame frame;
        if (!impl_->pending_events.empty()) {
            frame = std::move(impl_->pending_events.front());
            impl_->pending_events.pop_front();
        } else {
            frame = impl_->decoder.take_frame();
        }
        if (frame.header.type != MessageType::Event) {
            error = "unexpected non-EVENT frame on idle IPC session";
            return false;
        }
        std::uint64_t event_generation = 0;
        std::uint64_t sequence = 0;
        if (!json_u64(frame.payload, "generation", event_generation) ||
            !json_u64(frame.payload, "seq", sequence) || sequence == 0) {
            error = "invalid network service EVENT envelope";
            return false;
        }
        if (impl_->baseline_valid && event_generation == impl_->generation &&
            sequence <= impl_->snapshot_sequence) {
            continue;
        }
        if (!impl_->baseline_valid || event_generation != impl_->generation ||
            sequence != impl_->snapshot_sequence + 1U) {
            impl_->baseline_valid = false;
            impl_->rebase_required = true;
        } else {
            impl_->snapshot_sequence = sequence;
        }
        event_json = std::move(frame.payload);
        return true;
    }
    return false;
}

std::uint64_t NetworkServiceClient::generation() const { return impl_->generation; }
std::uint64_t NetworkServiceClient::snapshotSequence() const {
    return impl_->snapshot_sequence;
}
bool NetworkServiceClient::requiresSnapshotRebase() const {
    return impl_->rebase_required;
}

std::string jsonQuote(const std::string &value) {
    static const char hex[] = "0123456789abcdef";
    std::string out = "\"";
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
                out += "\\u00";
                out += hex[(ch >> 4U) & 0x0fU];
                out += hex[ch & 0x0fU];
            } else {
                out.push_back(static_cast<char>(ch));
            }
        }
    }
    out += '"';
    return out;
}

} // namespace network_service
