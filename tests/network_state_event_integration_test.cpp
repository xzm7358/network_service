#include "ipc/network_ipc_server.h"
#include "ipc/network_ipc_v1_codec.h"
#include "service/network_daemon.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <mutex>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

using network_service::ipc_v1::CodecError;
using network_service::ipc_v1::Frame;
using network_service::ipc_v1::FrameHeader;
using network_service::ipc_v1::MessageType;

void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

bool write_all(int fd, const void *data, std::size_t size) {
    const auto *bytes = static_cast<const unsigned char *>(data);
    std::size_t offset = 0;
    while (offset < size) {
#ifdef MSG_NOSIGNAL
        const ssize_t n = ::send(fd, bytes + offset, size - offset, MSG_NOSIGNAL);
#else
        const ssize_t n = ::send(fd, bytes + offset, size - offset, 0);
#endif
        if (n > 0) {
            offset += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

bool read_exact(int fd, void *data, std::size_t size) {
    auto *bytes = static_cast<unsigned char *>(data);
    std::size_t offset = 0;
    while (offset < size) {
        const ssize_t n = ::recv(fd, bytes + offset, size - offset, 0);
        if (n > 0) {
            offset += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

bool wait_readable(int fd, int timeout_ms) {
    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLIN;
    for (;;) {
        const int rc = ::poll(&pfd, 1, timeout_ms);
        if (rc > 0) return (pfd.revents & (POLLIN | POLLHUP)) != 0;
        if (rc == 0) return false;
        if (errno != EINTR) return false;
    }
}

Frame receive_frame(int fd, int timeout_ms = 1500) {
    require(wait_readable(fd, timeout_ms), "timed out waiting for v1 frame");
    std::uint8_t raw_header[network_service::ipc_v1::kHeaderSize]{};
    require(read_exact(fd, raw_header, sizeof(raw_header)), "failed to read v1 header");

    FrameHeader header;
    CodecError error = CodecError::None;
    require(network_service::ipc_v1::decode_header(
                raw_header, sizeof(raw_header), header, error),
            "invalid v1 header from server");

    Frame frame;
    frame.header = header;
    frame.payload.assign(header.payload_length, '\0');
    if (header.payload_length > 0) {
        require(read_exact(fd, frame.payload.data(), header.payload_length),
                "failed to read v1 payload");
    }
    return frame;
}

void send_frame(int fd, MessageType type, const std::string &payload) {
    CodecError error = CodecError::None;
    const auto encoded = network_service::ipc_v1::encode_frame(type, payload, error);
    require(error == CodecError::None && !encoded.empty(), "failed to encode client frame");
    require(write_all(fd, encoded.data(), encoded.size()), "failed to send client frame");
}

std::uint64_t json_u64(const std::string &json, const char *key) {
    const std::string needle = std::string("\"") + key + "\":";
    std::size_t pos = json.find(needle);
    require(pos != std::string::npos, "required uint64 JSON member missing");
    pos += needle.size();
    std::uint64_t value = 0;
    bool any = false;
    while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
        any = true;
        const unsigned digit = static_cast<unsigned>(json[pos] - '0');
        require(value <= (UINT64_MAX - digit) / 10U, "uint64 JSON member overflow");
        value = value * 10U + digit;
        ++pos;
    }
    require(any, "required uint64 JSON member invalid");
    return value;
}

struct Client {
    int fd = -1;
    std::uint64_t generation = 0;

    Client() = default;
    Client(const Client &) = delete;
    Client &operator=(const Client &) = delete;
    Client(Client &&other) noexcept : fd(other.fd), generation(other.generation) {
        other.fd = -1;
    }
    Client &operator=(Client &&other) noexcept {
        if (this == &other) return *this;
        if (fd >= 0) ::close(fd);
        fd = other.fd;
        generation = other.generation;
        other.fd = -1;
        return *this;
    }
    ~Client() {
        if (fd >= 0) ::close(fd);
    }
};

int connect_unix(const std::string &path) {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    require(fd >= 0, "client socket failed");
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    require(path.size() < sizeof(addr.sun_path), "client socket path too long");
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1U);
    if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        throw std::runtime_error("client connect failed");
    }
    return fd;
}

Client connect_ready(const std::string &path, const char *client_name) {
    Client client;
    client.fd = connect_unix(path);
    send_frame(client.fd,
               MessageType::Hello,
               std::string("{\"minVersion\":1,\"maxVersion\":1,\"client\":\"") +
                   client_name + "\",\"capabilities\":[]}");
    const Frame ready = receive_frame(client.fd);
    require(ready.header.type == MessageType::Ready, "HELLO did not produce READY");
    client.generation = json_u64(ready.payload, "generation");
    require(client.generation != 0, "READY generation is zero");
    return client;
}

std::uint64_t request_snapshot(Client &client, std::uint64_t request_id,
                               std::string *payload_out = nullptr) {
    send_frame(client.fd,
               MessageType::Request,
               "{\"requestId\":" + std::to_string(request_id) +
                   ",\"method\":\"network.snapshot\",\"params\":{}}");
    const Frame response = receive_frame(client.fd);
    require(response.header.type == MessageType::Response,
            "snapshot did not return RESPONSE");
    require(json_u64(response.payload, "requestId") == request_id,
            "snapshot requestId mismatch");
    require(json_u64(response.payload, "status") == 200,
            "snapshot status is not 200");
    require(json_u64(response.payload, "generation") == client.generation,
            "snapshot generation mismatch");
    if (payload_out != nullptr) *payload_out = response.payload;
    return json_u64(response.payload, "snapshotSeq");
}

std::uint64_t subscribe(Client &client, std::uint64_t request_id) {
    send_frame(client.fd,
               MessageType::Request,
               "{\"requestId\":" + std::to_string(request_id) +
                   ",\"method\":\"network.events.subscribe\",\"params\":{}}");
    const Frame response = receive_frame(client.fd);
    require(response.header.type == MessageType::Response,
            "subscribe did not return RESPONSE");
    require(json_u64(response.payload, "requestId") == request_id,
            "subscribe requestId mismatch");
    require(response.payload.find("\"subscribed\":true") != std::string::npos,
            "subscribe result mismatch");

    const Frame event = receive_frame(client.fd);
    require(event.header.type == MessageType::Event,
            "subscribe did not emit control EVENT");
    require(event.payload.find("\"event\":\"network.events.subscribed\"") !=
                std::string::npos,
            "unexpected subscription control EVENT");
    require(json_u64(event.payload, "generation") == client.generation,
            "subscription EVENT generation mismatch");
    return json_u64(event.payload, "seq");
}

network_service::NetworkSnapshot disconnected_snapshot() {
    network_service::NetworkSnapshot snapshot;
    snapshot.eth.iface = "eth0";
    snapshot.eth.exists = true;
    snapshot.eth.enabled = true;
    snapshot.eth.route_metric = 10;
    snapshot.wifi.iface = "wlan0";
    snapshot.wifi.exists = true;
    snapshot.wifi.enabled = true;
    snapshot.wifi.route_metric = 20;
    snapshot.wifi.signal_dbm = -80;
    snapshot.wifi.signal_bars = 1;
    return snapshot;
}

network_service::NetworkSnapshot connected_wifi_snapshot() {
    auto snapshot = disconnected_snapshot();
    snapshot.wifi.carrier_up = true;
    snapshot.wifi.connected = true;
    snapshot.wifi.has_ip = true;
    snapshot.wifi.ip4 = "192.168.50.20";
    snapshot.wifi.netmask4 = "255.255.255.0";
    snapshot.wifi.has_default_route = true;
    snapshot.wifi.gateway4 = "192.168.50.1";
    snapshot.wifi.ssid = "FixtureWifi";
    snapshot.wifi.signal_dbm = -48;
    snapshot.wifi.signal_bars = 4;
    snapshot.primary_iface = "wlan0";
    snapshot.dns_available = true;
    snapshot.dns4 = "1.1.1.1";
    snapshot.online = true;
    return snapshot;
}

class SnapshotFixture {
public:
    explicit SnapshotFixture(network_service::NetworkSnapshot initial)
        : snapshot_(std::move(initial)) {}

    network_service::NetworkSnapshot get() const {
        std::lock_guard<std::mutex> guard(lock_);
        return snapshot_;
    }

    void set(network_service::NetworkSnapshot snapshot) {
        std::lock_guard<std::mutex> guard(lock_);
        snapshot_ = std::move(snapshot);
    }

private:
    mutable std::mutex lock_;
    network_service::NetworkSnapshot snapshot_;
};

class ServerHarness {
public:
    explicit ServerHarness(network_service::NetworkSnapshot initial)
        : fixture_(std::move(initial)),
          daemon_("eth0", "wlan0", "/tmp", "/tmp/ns-no-wpa-control",
                  [this]() { return fixture_.get(); }),
          server_(daemon_) {
        static unsigned counter = 0;
        socket_path_ = "/tmp/ns_state_event_" + std::to_string(::getpid()) + "_" +
                       std::to_string(counter++) + ".sock";
        require(server_.listen(socket_path_), "server listen failed");
        worker_ = std::thread([this]() { server_.run(); });
    }

    ~ServerHarness() {
        if (server_.wake_write_fd() >= 0) {
            const char wake = 'x';
            (void)::write(server_.wake_write_fd(), &wake, 1);
        }
        if (worker_.joinable()) worker_.join();
        server_.stop();
    }

    const std::string &socket_path() const { return socket_path_; }
    void set_snapshot(network_service::NetworkSnapshot snapshot) {
        fixture_.set(std::move(snapshot));
    }

private:
    SnapshotFixture fixture_;
    network_service::NetworkDaemon daemon_;
    network_service::NetworkIpcServer server_;
    std::string socket_path_;
    std::thread worker_;
};

void test_state_change_advances_watermark_without_subscription() {
    ServerHarness harness(disconnected_snapshot());
    Client client = connect_ready(harness.socket_path(), "watermark-client");
    const std::uint64_t initial_seq = request_snapshot(client, 101);
    require(initial_seq == 0, "initial snapshotSeq must be zero");

    harness.set_snapshot(connected_wifi_snapshot());
    std::uint64_t observed_seq = initial_seq;
    std::string snapshot_payload;
    for (int i = 0; i < 30 && observed_seq == initial_seq; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        observed_seq = request_snapshot(client, 102 + static_cast<std::uint64_t>(i),
                                        &snapshot_payload);
    }
    require(observed_seq == 1,
            "state transition without subscribers must allocate one EVENT sequence");
    require(snapshot_payload.find("192.168.50.20") != std::string::npos,
            "authoritative snapshot did not reflect fixture transition");

    const std::uint64_t control_seq = subscribe(client, 200);
    require(control_seq == observed_seq + 1,
            "subscription control EVENT is not continuous with snapshot watermark");
}

void test_multi_client_control_and_dynamic_event_continuity() {
    ServerHarness harness(disconnected_snapshot());

    Client first = connect_ready(harness.socket_path(), "subscriber-a");
    require(request_snapshot(first, 301) == 0, "first baseline snapshotSeq changed");
    const std::uint64_t first_control = subscribe(first, 302);
    require(first_control == 1, "first control EVENT seq changed");

    Client second = connect_ready(harness.socket_path(), "subscriber-b");
    require(second.generation == first.generation, "clients do not share server generation");
    const std::uint64_t second_baseline = request_snapshot(second, 401);
    require(second_baseline == first_control,
            "second client snapshot did not inherit global watermark");
    const std::uint64_t second_control = subscribe(second, 402);
    require(second_control == first_control + 1,
            "second subscription control seq is not globally continuous");

    const Frame mirrored_control = receive_frame(first.fd);
    require(mirrored_control.header.type == MessageType::Event,
            "existing subscriber did not receive global control EVENT");
    require(mirrored_control.payload.find("network.events.subscribed") != std::string::npos,
            "existing subscriber received wrong mirrored control EVENT");
    require(json_u64(mirrored_control.payload, "seq") == second_control,
            "mirrored control EVENT seq differs between subscribers");

    harness.set_snapshot(connected_wifi_snapshot());
    const Frame first_dynamic = receive_frame(first.fd, 2000);
    const Frame second_dynamic = receive_frame(second.fd, 2000);
    require(first_dynamic.header.type == MessageType::Event &&
                second_dynamic.header.type == MessageType::Event,
            "state transition did not produce EVENT for both subscribers");
    require(first_dynamic.payload.find("\"event\":\"network.state.changed\"") !=
                std::string::npos,
            "first subscriber dynamic event name mismatch");
    require(second_dynamic.payload.find("\"event\":\"network.state.changed\"") !=
                std::string::npos,
            "second subscriber dynamic event name mismatch");

    const std::uint64_t first_seq = json_u64(first_dynamic.payload, "seq");
    const std::uint64_t second_seq = json_u64(second_dynamic.payload, "seq");
    require(first_seq == second_control + 1 && first_seq == second_seq,
            "one transition did not produce one shared global EVENT seq");
    require(first_dynamic.payload.find(
                "\"changed\":[\"wifi\",\"route\",\"dns\"]") !=
                std::string::npos,
            "dynamic invalidation payload categories changed");

    std::string snapshot_payload;
    const std::uint64_t rebased_seq = request_snapshot(first, 303, &snapshot_payload);
    require(rebased_seq == first_seq,
            "snapshotSeq is not aligned with delivered state EVENT seq");
    require(snapshot_payload.find("FixtureWifi") != std::string::npos,
            "snapshot after EVENT does not contain authoritative Wi-Fi state");

    require(!wait_readable(first.fd, 400),
            "identical consecutive snapshots emitted a duplicate EVENT");
}

} // namespace

int main() {
    try {
        test_state_change_advances_watermark_without_subscription();
        test_multi_client_control_and_dynamic_event_continuity();
        std::cout << "Network state EVENT integration contracts passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "Network state EVENT integration test failed: " << e.what() << '\n';
        return 1;
    }
}
