#include "platform/wpa_event_monitor.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <mutex>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

namespace {

bool expect(bool condition, const char *message) {
    if (condition) return true;
    std::cerr << "wpa_event_monitor_test: " << message << std::endl;
    return false;
}

bool bind_server(int fd, const std::string &path) {
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) return false;
    std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());
    unlink(path.c_str());
    return bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0;
}

bool send_reply(int fd,
                const sockaddr_un &peer,
                socklen_t peer_len,
                const std::string &reply) {
    return sendto(fd,
                  reply.data(),
                  reply.size(),
                  0,
                  reinterpret_cast<const sockaddr *>(&peer),
                  peer_len) == static_cast<ssize_t>(reply.size());
}

bool receive_command(int fd,
                     std::string &command,
                     sockaddr_un &peer,
                     socklen_t &peer_len,
                     int timeout_ms = 3000) {
    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLIN;
    const int rc = poll(&pfd, 1, timeout_ms);
    if (rc <= 0 || !(pfd.revents & POLLIN)) return false;
    char buffer[512];
    peer_len = sizeof(peer);
    const ssize_t n = recvfrom(fd,
                               buffer,
                               sizeof(buffer),
                               0,
                               reinterpret_cast<sockaddr *>(&peer),
                               &peer_len);
    if (n <= 0) return false;
    command.assign(buffer, static_cast<std::size_t>(n));
    return true;
}

template <typename Predicate>
bool wait_until(Predicate predicate, int timeout_ms = 3000) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return predicate();
}

} // namespace

int main() {
    using network_service::WifiL2State;
    using network_service::WpaStatusFact;

    bool ok = true;

    {
        WpaStatusFact status;
        ok = expect(network_service::parse_wpa_status_reply(
                        "bssid=00:11:22:33:44:55\nssid=LabAP\nwpa_state=COMPLETED\n",
                        status),
                    "COMPLETED STATUS parse failed") && ok;
        ok = expect(status.l2_state == WifiL2State::Connected &&
                    status.ssid == "LabAP" &&
                    status.bssid == "00:11:22:33:44:55",
                    "COMPLETED STATUS projection mismatch") && ok;

        ok = expect(network_service::parse_wpa_status_reply(
                        "ssid=\\xe4\\xb8\\xad\\xe6\\x96\\x87 AP\n"
                        "wpa_state=COMPLETED\n",
                        status) && status.ssid == u8"中文 AP",
                    "escaped UTF-8 STATUS SSID was not decoded") && ok;

        ok = expect(network_service::parse_wpa_status_reply(
                        "wpa_state=4WAY_HANDSHAKE\n", status) &&
                    status.l2_state == WifiL2State::Handshake,
                    "handshake STATUS projection mismatch") && ok;
        ok = expect(network_service::parse_wpa_status_reply(
                        "wpa_state=DISCONNECTED\n", status) &&
                    status.l2_state == WifiL2State::Disconnected,
                    "disconnected STATUS projection mismatch") && ok;
        ok = expect(network_service::parse_wpa_status_reply(
                        "wpa_state=INTERFACE_DISABLED\n", status) &&
                    status.l2_state == WifiL2State::Disabled,
                    "disabled STATUS projection mismatch") && ok;
        ok = expect(!network_service::parse_wpa_status_reply("ssid=no-state\n", status),
                    "STATUS without wpa_state must be rejected") && ok;
    }

    const std::string ctrl_dir =
        "/tmp/network_service_wpa_event_test_" + std::to_string(getpid());
    const std::string iface = "wlan0";
    const std::string ctrl_path = ctrl_dir + "/" + iface;
    (void)mkdir(ctrl_dir.c_str(), 0755);

    const int server_fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (!expect(server_fd >= 0, "failed to create fake supplicant socket")) return 1;
    if (!expect(bind_server(server_fd, ctrl_path), "failed to bind fake supplicant socket")) {
        close(server_fd);
        rmdir(ctrl_dir.c_str());
        return 1;
    }

    std::mutex gate_lock;
    std::condition_variable gate_cv;
    bool send_disconnect = false;
    std::atomic<bool> server_ok{true};

    std::thread server([&]() {
        sockaddr_un event_peer{};
        socklen_t event_peer_len = sizeof(event_peer);
        sockaddr_un peer{};
        socklen_t peer_len = sizeof(peer);
        std::string command;

        if (!receive_command(server_fd, command, event_peer, event_peer_len) ||
            command != "ATTACH" ||
            !send_reply(server_fd, event_peer, event_peer_len, "OK\n")) {
            server_ok = false;
            return;
        }

        if (!receive_command(server_fd, command, peer, peer_len) ||
            command != "STATUS" ||
            !send_reply(server_fd,
                        peer,
                        peer_len,
                        "bssid=00:11:22:33:44:55\n"
                        "ssid=BrownfieldAP\n"
                        "wpa_state=COMPLETED\n")) {
            server_ok = false;
            return;
        }

        {
            std::unique_lock<std::mutex> lock(gate_lock);
            gate_cv.wait_for(lock,
                             std::chrono::seconds(3),
                             [&]() { return send_disconnect; });
            if (!send_disconnect) {
                server_ok = false;
                return;
            }
        }

        if (!send_reply(server_fd,
                        event_peer,
                        event_peer_len,
                        "<3>CTRL-EVENT-DISCONNECTED bssid=00:11:22:33:44:55 reason=3")) {
            server_ok = false;
            return;
        }

        // Monitor shutdown detaches the event socket. Reply so close() does not
        // wait for its timeout and the test remains deterministic.
        if (!receive_command(server_fd, command, peer, peer_len, 5000) ||
            command != "DETACH" ||
            !send_reply(server_fd, peer, peer_len, "OK\n")) {
            server_ok = false;
        }
    });

    std::atomic<int> connected_callbacks{0};
    std::atomic<int> disconnected_callbacks{0};
    network_service::WpaEventMonitor monitor(
        iface,
        ctrl_dir,
        [&](bool connected) {
            if (connected) ++connected_callbacks;
            else ++disconnected_callbacks;
        });

    monitor.start();
    ok = expect(wait_until([&]() {
                    const auto snapshot = monitor.snapshot();
                    return snapshot.attached &&
                           snapshot.l2_state == WifiL2State::Connected &&
                           snapshot.last_ssid == "BrownfieldAP" &&
                           snapshot.last_bssid == "00:11:22:33:44:55" &&
                           connected_callbacks.load() == 1;
                }),
                "ATTACH STATUS resync did not publish current connected truth") && ok;

    {
        std::lock_guard<std::mutex> lock(gate_lock);
        send_disconnect = true;
    }
    gate_cv.notify_all();

    ok = expect(wait_until([&]() {
                    const auto snapshot = monitor.snapshot();
                    return snapshot.l2_state == WifiL2State::Disconnected &&
                           disconnected_callbacks.load() == 1;
                }),
                "post-resync DISCONNECTED event did not update L2 truth") && ok;

    monitor.stop();
    server.join();
    close(server_fd);
    unlink(ctrl_path.c_str());
    rmdir(ctrl_dir.c_str());

    ok = expect(server_ok.load(), "fake supplicant STATUS/event sequence failed") && ok;
    return ok ? 0 : 1;
}
