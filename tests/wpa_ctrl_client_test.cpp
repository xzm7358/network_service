#include "platform/wpa_ctrl_client.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

namespace {

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

bool expect(bool condition, const char *message) {
    if (condition) return true;
    std::cerr << "wpa_ctrl_client_test: " << message << std::endl;
    return false;
}

} // namespace

int main() {
    const std::string server_path =
        "/tmp/network_service_wpa_ctrl_test_" + std::to_string(getpid()) + ".sock";

    int server_fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (!expect(server_fd >= 0, "failed to create fake supplicant socket")) return 1;
    if (!expect(bind_server(server_fd, server_path), "failed to bind fake supplicant socket")) {
        close(server_fd);
        return 1;
    }

    std::atomic<bool> server_ok{true};
    std::thread server([&]() {
        sockaddr_un peer{};
        socklen_t peer_len = sizeof(peer);
        char buffer[256];

        auto receive_command = [&](std::string &command) -> bool {
            pollfd pfd{};
            pfd.fd = server_fd;
            pfd.events = POLLIN;
            int rc = poll(&pfd, 1, 2000);
            if (rc <= 0 || !(pfd.revents & POLLIN)) return false;
            peer_len = sizeof(peer);
            ssize_t n = recvfrom(server_fd,
                                 buffer,
                                 sizeof(buffer),
                                 0,
                                 reinterpret_cast<sockaddr *>(&peer),
                                 &peer_len);
            if (n <= 0) return false;
            command.assign(buffer, static_cast<std::size_t>(n));
            return true;
        };

        std::string command;
        if (!receive_command(command) || command != "PING" ||
            !send_reply(server_fd, peer, peer_len, "PONG\n")) {
            server_ok = false;
            return;
        }

        if (!receive_command(command) || command != "ATTACH" ||
            !send_reply(server_fd, peer, peer_len, "OK\n") ||
            !send_reply(server_fd,
                        peer,
                        peer_len,
                        "<3>CTRL-EVENT-CONNECTED - Connection to 00:11:22:33:44:55 completed")) {
            server_ok = false;
            return;
        }

        if (!receive_command(command) || command != "DETACH" ||
            !send_reply(server_fd, peer, peer_len, "OK\n")) {
            server_ok = false;
        }
    });

    bool ok = true;
    {
        network_service::WpaCtrlClient client(server_path);
        std::string error;
        std::string reply;

        ok = expect(client.open(error), error.c_str()) && ok;
        ok = expect(client.is_open(), "client should report open") && ok;
        ok = expect(client.request("PING", reply, error), error.c_str()) && ok;
        ok = expect(reply == "PONG\n", "unexpected PING reply") && ok;
        ok = expect(client.attach(error), error.c_str()) && ok;

        pollfd pfd{};
        pfd.fd = client.fd();
        pfd.events = POLLIN;
        int rc = poll(&pfd, 1, 2000);
        ok = expect(rc > 0 && (pfd.revents & POLLIN), "expected attached WPA event") && ok;

        std::string event;
        if (rc > 0 && (pfd.revents & POLLIN)) {
            ok = expect(client.receive(event, error), error.c_str()) && ok;
            ok = expect(event.find("CTRL-EVENT-CONNECTED") != std::string::npos,
                        "unexpected attached event") && ok;
        }

        client.close();
        ok = expect(!client.is_open(), "client should report closed") && ok;
    }

    server.join();
    close(server_fd);
    unlink(server_path.c_str());

    ok = expect(server_ok.load(), "fake supplicant command sequence failed") && ok;
    return ok ? 0 : 1;
}
