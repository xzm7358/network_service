#include "ipc/network_ipc_v1_codec.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

using network_service::ipc_v1::CodecError;
using network_service::ipc_v1::Frame;
using network_service::ipc_v1::FrameHeader;
using network_service::ipc_v1::MessageType;
using Clock = std::chrono::steady_clock;

struct Options {
    std::string socket_path = "/tmp/smart_hmi_network.sock";
    std::string command;
    std::string out_path;
    std::string out_prefix;
    int timeout_ms = 1500;
    int poll_ms = 100;
    int scan_timeout_ms = 10000;
};

[[noreturn]] void fail(const std::string &message) {
    throw std::runtime_error(message);
}

std::int64_t monotonic_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               Clock::now().time_since_epoch())
        .count();
}

bool parse_positive_int(const std::string &text, int *out) {
    if (out == nullptr || text.empty()) return false;
    long value = 0;
    for (char c : text) {
        if (c < '0' || c > '9') return false;
        value = value * 10L + static_cast<long>(c - '0');
        if (value > INT_MAX) return false;
    }
    if (value <= 0) return false;
    *out = static_cast<int>(value);
    return true;
}

void usage(const char *argv0) {
    std::cerr
        << "Usage: " << argv0
        << " [--socket PATH] [--timeout-ms N] [--poll-ms N] [--scan-timeout-ms N]"
           " [--out PATH] [--out-prefix PATH]"
           " {ready|snapshot|scan-cycle|wait-ready|clock-ms}\n";
}

bool parse_args(int argc, char **argv, Options *options) {
    if (options == nullptr) return false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&](std::string *out) -> bool {
            if (out == nullptr || i + 1 >= argc) return false;
            *out = argv[++i];
            return !out->empty();
        };
        auto number = [&](int *out) -> bool {
            std::string text;
            return value(&text) && parse_positive_int(text, out);
        };

        if (arg == "--socket") {
            if (!value(&options->socket_path)) return false;
        } else if (arg == "--timeout-ms") {
            if (!number(&options->timeout_ms)) return false;
        } else if (arg == "--poll-ms") {
            if (!number(&options->poll_ms)) return false;
        } else if (arg == "--scan-timeout-ms") {
            if (!number(&options->scan_timeout_ms)) return false;
        } else if (arg == "--out") {
            if (!value(&options->out_path)) return false;
        } else if (arg == "--out-prefix") {
            if (!value(&options->out_prefix)) return false;
        } else if (arg == "--help" || arg == "-h") {
            usage(argv[0]);
            return false;
        } else if (!arg.empty() && arg[0] == '-') {
            return false;
        } else if (options->command.empty()) {
            options->command = arg;
        } else {
            return false;
        }
    }
    return !options->command.empty();
}

bool wait_fd(int fd, short events, std::int64_t deadline_ms) {
    for (;;) {
        const std::int64_t remaining = deadline_ms - monotonic_ms();
        if (remaining <= 0) return false;
        pollfd pfd{};
        pfd.fd = fd;
        pfd.events = events;
        const int rc = ::poll(
            &pfd, 1, static_cast<int>(std::min<std::int64_t>(remaining, INT_MAX)));
        if (rc > 0) {
            if ((pfd.revents & events) != 0) return true;
            if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) return false;
            continue;
        }
        if (rc == 0) return false;
        if (errno != EINTR) return false;
    }
}

bool write_all(int fd, const void *data, std::size_t size, std::int64_t deadline_ms) {
    const auto *bytes = static_cast<const unsigned char *>(data);
    std::size_t offset = 0;
    while (offset < size) {
        if (!wait_fd(fd, POLLOUT, deadline_ms)) return false;
#ifdef MSG_NOSIGNAL
        const ssize_t n = ::send(fd, bytes + offset, size - offset, MSG_NOSIGNAL);
#else
        const ssize_t n = ::send(fd, bytes + offset, size - offset, 0);
#endif
        if (n > 0) {
            offset += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            continue;
        }
        return false;
    }
    return true;
}

bool read_exact(int fd, void *data, std::size_t size, std::int64_t deadline_ms) {
    auto *bytes = static_cast<unsigned char *>(data);
    std::size_t offset = 0;
    while (offset < size) {
        if (!wait_fd(fd, POLLIN, deadline_ms)) return false;
        const ssize_t n = ::recv(fd, bytes + offset, size - offset, 0);
        if (n > 0) {
            offset += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            continue;
        }
        return false;
    }
    return true;
}

int connect_unix(const std::string &path, int timeout_ms) {
    sockaddr_un addr{};
    if (path.empty() || path.size() >= sizeof(addr.sun_path)) return -1;

    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        ::close(fd);
        return -1;
    }

    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1U);
    if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0) return fd;
    if (errno != EINPROGRESS) {
        ::close(fd);
        return -1;
    }

    const std::int64_t deadline = monotonic_ms() + timeout_ms;
    if (!wait_fd(fd, POLLOUT, deadline)) {
        ::close(fd);
        return -1;
    }
    int socket_error = 0;
    socklen_t length = sizeof(socket_error);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &length) != 0 ||
        socket_error != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

void send_frame(int fd, MessageType type, const std::string &payload, int timeout_ms) {
    CodecError error = CodecError::None;
    const std::vector<std::uint8_t> encoded =
        network_service::ipc_v1::encode_frame(type, payload, error);
    if (error != CodecError::None || encoded.empty()) fail("failed to encode v1 frame");
    if (!write_all(fd, encoded.data(), encoded.size(), monotonic_ms() + timeout_ms)) {
        fail("failed to write v1 frame");
    }
}

Frame receive_frame(int fd, int timeout_ms) {
    const std::int64_t deadline = monotonic_ms() + timeout_ms;
    std::uint8_t header_bytes[network_service::ipc_v1::kHeaderSize]{};
    if (!read_exact(fd, header_bytes, sizeof(header_bytes), deadline)) {
        fail("timed out or disconnected while reading v1 header");
    }

    FrameHeader header;
    CodecError error = CodecError::None;
    if (!network_service::ipc_v1::decode_header(
            header_bytes, sizeof(header_bytes), header, error)) {
        fail(std::string("invalid v1 header: ") +
             network_service::ipc_v1::codec_error_string(error));
    }

    Frame frame;
    frame.header = header;
    frame.payload.assign(header.payload_length, '\0');
    if (header.payload_length > 0 &&
        !read_exact(fd, frame.payload.data(), header.payload_length, deadline)) {
        fail("timed out or disconnected while reading v1 payload");
    }
    return frame;
}

bool contains_json_string(const std::string &json,
                          const std::string &key,
                          const std::string &value) {
    const std::string needle = "\"" + key + "\":\"" + value + "\"";
    return json.find(needle) != std::string::npos;
}

bool contains_capability(const std::string &json, const char *capability) {
    return json.find(std::string("\"") + capability + "\"") != std::string::npos;
}

std::uint64_t json_u64(const std::string &json, const char *key) {
    const std::string needle = std::string("\"") + key + "\":";
    const std::size_t found = json.find(needle);
    if (found == std::string::npos) fail(std::string("missing uint64 field: ") + key);
    std::size_t pos = found + needle.size();
    std::uint64_t value = 0;
    bool any = false;
    while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
        any = true;
        const unsigned digit = static_cast<unsigned>(json[pos] - '0');
        if (value > (UINT64_MAX - digit) / 10U) fail("uint64 field overflow");
        value = value * 10U + digit;
        ++pos;
    }
    if (!any) fail(std::string("invalid uint64 field: ") + key);
    return value;
}

int json_status(const std::string &json) {
    const std::uint64_t value = json_u64(json, "status");
    if (value > static_cast<std::uint64_t>(INT_MAX)) fail("status exceeds INT_MAX");
    return static_cast<int>(value);
}

std::string scan_state(const std::string &json) {
    for (const char *state : {"idle", "scanning", "ready", "failed"}) {
        if (contains_json_string(json, "state", state)) return state;
    }
    fail("scan response has no recognized state");
}

void write_text(const std::string &path, const std::string &text) {
    if (path.empty()) return;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) fail("failed to open output file: " + path);
    out << text << '\n';
    if (!out) fail("failed to write output file: " + path);
}

class Session {
public:
    Session(std::string socket_path, int timeout_ms)
        : socket_path_(std::move(socket_path)), timeout_ms_(timeout_ms) {}

    ~Session() {
        if (fd_ >= 0) ::close(fd_);
    }

    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;

    std::int64_t connect_ready() {
        const std::int64_t start = monotonic_ms();
        fd_ = connect_unix(socket_path_, timeout_ms_);
        if (fd_ < 0) fail("failed to connect AF_UNIX socket");
        send_frame(
            fd_, MessageType::Hello,
            R"({"minVersion":1,"maxVersion":1,"client":"network_service_rc_probe","capabilities":[]})",
            timeout_ms_);
        const Frame ready = receive_frame(fd_, timeout_ms_);
        if (ready.header.type != MessageType::Ready) fail("HELLO did not produce READY");
        if (!contains_json_string(ready.payload, "service", "network_service") ||
            json_u64(ready.payload, "version") != 1 ||
            json_u64(ready.payload, "generation") == 0) {
            fail("READY envelope is invalid");
        }
        for (const char *capability : {"events", "snapshot-rebase", "network-control"}) {
            if (!contains_capability(ready.payload, capability)) {
                fail(std::string("READY missing required capability: ") + capability);
            }
        }
        ready_payload_ = ready.payload;
        return monotonic_ms() - start;
    }

    const std::string &ready_payload() const { return ready_payload_; }

    std::string request(const std::string &method,
                        const std::string &params,
                        std::int64_t *latency_ms = nullptr) {
        if (fd_ < 0) fail("request requires READY session");
        const std::uint64_t request_id = next_request_id_++;
        const std::string payload =
            "{\"requestId\":" + std::to_string(request_id) +
            ",\"method\":\"" + method + "\",\"params\":" + params + "}";
        const std::int64_t start = monotonic_ms();
        send_frame(fd_, MessageType::Request, payload, timeout_ms_);
        for (;;) {
            const Frame frame = receive_frame(fd_, timeout_ms_);
            if (frame.header.type == MessageType::Event) continue;
            if (frame.header.type == MessageType::Error) {
                fail("server returned ERROR: " + frame.payload);
            }
            if (frame.header.type != MessageType::Response) {
                fail("unexpected frame while waiting for RESPONSE");
            }
            if (json_u64(frame.payload, "requestId") != request_id) {
                fail("requestId correlation mismatch");
            }
            if (latency_ms != nullptr) *latency_ms = monotonic_ms() - start;
            return frame.payload;
        }
    }

private:
    std::string socket_path_;
    int timeout_ms_ = 1500;
    int fd_ = -1;
    std::uint64_t next_request_id_ = 1;
    std::string ready_payload_;
};

int command_ready(const Options &options) {
    Session session(options.socket_path, options.timeout_ms);
    const std::int64_t latency = session.connect_ready();
    write_text(options.out_path, session.ready_payload());
    std::cout << "ready_latency_ms=" << latency << '\n';
    return 0;
}

int command_snapshot(const Options &options) {
    Session session(options.socket_path, options.timeout_ms);
    (void)session.connect_ready();
    std::int64_t latency = 0;
    const std::string response = session.request("network.snapshot", "{}", &latency);
    if (json_status(response) != 200) fail("network.snapshot did not return status 200");
    write_text(options.out_path, response);
    std::cout << "snapshot_latency_ms=" << latency << '\n';
    return 0;
}

int command_wait_ready(const Options &options) {
    const std::int64_t start = monotonic_ms();
    const std::int64_t deadline = start + options.scan_timeout_ms;
    std::string last_error;
    while (monotonic_ms() < deadline) {
        try {
            Session session(options.socket_path, options.timeout_ms);
            (void)session.connect_ready();
            write_text(options.out_path, session.ready_payload());
            std::cout << "ready_latency_ms=" << (monotonic_ms() - start) << '\n';
            return 0;
        } catch (const std::exception &e) {
            last_error = e.what();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(options.poll_ms));
    }
    fail("READY not recovered before timeout: " + last_error);
}

int command_scan_cycle(const Options &options) {
    Session session(options.socket_path, options.timeout_ms);
    (void)session.connect_ready();

    const std::int64_t cycle_start = monotonic_ms();
    std::int64_t start_latency = 0;
    const std::string start_response =
        session.request("wifi.scan.start", "{}", &start_latency);
    const int start_status = json_status(start_response);
    if (start_status != 202 && start_status != 200) {
        fail("wifi.scan.start did not return 202/200");
    }
    const std::uint64_t scan_id = json_u64(start_response, "scanId");
    if (!options.out_prefix.empty()) {
        write_text(options.out_prefix + ".start.json", start_response);
    }

    const std::int64_t deadline = cycle_start + options.scan_timeout_ms;
    std::string final_response;
    std::string state = scan_state(start_response);
    while (state == "scanning" && monotonic_ms() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(options.poll_ms));
        final_response = session.request("wifi.scan.status", "{}");
        if (json_status(final_response) != 200) {
            fail("wifi.scan.status did not return status 200");
        }
        if (json_u64(final_response, "scanId") != scan_id) {
            fail("wifi.scan.status scanId mismatch");
        }
        state = scan_state(final_response);
    }
    if (final_response.empty()) final_response = start_response;
    if (!options.out_prefix.empty()) {
        write_text(options.out_prefix + ".final.json", final_response);
    }

    const std::int64_t completion = monotonic_ms() - cycle_start;
    std::cout << scan_id << ',' << start_latency << ',' << completion << ',' << state
              << '\n';
    if (state != "ready") {
        fail("physical scan did not reach ready state");
    }
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    Options options;
    if (!parse_args(argc, argv, &options)) {
        usage(argv[0]);
        return 2;
    }

    try {
        if (options.command == "clock-ms") {
            std::cout << monotonic_ms() << '\n';
            return 0;
        }
        if (options.command == "ready") return command_ready(options);
        if (options.command == "snapshot") return command_snapshot(options);
        if (options.command == "wait-ready") return command_wait_ready(options);
        if (options.command == "scan-cycle") return command_scan_cycle(options);
        fail("unknown command: " + options.command);
    } catch (const std::exception &e) {
        std::cerr << "network_service_rc_probe: " << e.what() << '\n';
        return 1;
    }
}
