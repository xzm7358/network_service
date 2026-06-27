#include <csignal>
#include <iostream>
#include <string>
#include <unistd.h>

#ifndef NETWORK_SERVICE_VERSION
#define NETWORK_SERVICE_VERSION "0.1.0"
#endif

#include "ipc/network_ipc_server.h"
#include "network_service_protocol.h"
#include "service/network_daemon.h"

namespace {

void handle_signal(int) {
    const char wake = 'x';
    int fd = network_service::NetworkIpcServer::wake_fd();
    if (fd >= 0) {
        (void)write(fd, &wake, sizeof(wake));
    }
}

struct Options {
    std::string socket_path = network_service::kDefaultSocketPath;
    std::string eth_iface = "eth0";
    std::string wifi_iface = "wlan0";
    std::string config_dir = "/dnake/data";
    std::string event_dir = "/var/run/wpa_supplicant";
};

void print_usage(const char *argv0) {
    std::cerr << "Usage: " << argv0
              << " [--socket PATH] [--eth IFACE] [--wifi IFACE] [--config-dir DIR] [--event-dir DIR]\n";
}

bool parse_args(int argc, char **argv, Options &options) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto require_value = [&](std::string &out) -> bool {
            if (i + 1 >= argc) return false;
            out = argv[++i];
            return !out.empty();
        };

        if (arg == "--socket") {
            if (!require_value(options.socket_path)) return false;
        } else if (arg == "--eth") {
            if (!require_value(options.eth_iface)) return false;
        } else if (arg == "--wifi") {
            if (!require_value(options.wifi_iface)) return false;
        } else if (arg == "--config-dir") {
            if (!require_value(options.config_dir)) return false;
        } else if (arg == "--event-dir") {
            if (!require_value(options.event_dir)) return false;
        } else if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return false;
        } else {
            std::cerr << "Unknown argument: " << arg << std::endl;
            return false;
        }
    }
    return true;
}

} // namespace

int main(int argc, char **argv) {
    Options options;
    if (!parse_args(argc, argv, options)) {
        return 1;
    }

    signal(SIGTERM, handle_signal);
    signal(SIGINT, handle_signal);
#ifdef SIGPIPE
    signal(SIGPIPE, SIG_IGN);
#endif

    network_service::NetworkDaemon daemon(options.eth_iface, options.wifi_iface, options.config_dir, options.event_dir);
    network_service::NetworkIpcServer server(daemon);
    if (server.wake_write_fd() < 0) {
        std::cerr << "network_service: failed to create signal wake pipe" << std::endl;
        return 2;
    }
    network_service::NetworkIpcServer::set_wake_fd(server.wake_write_fd());

    if (!server.listen(options.socket_path)) {
        network_service::NetworkIpcServer::set_wake_fd(-1);
        return 2;
    }

    std::cout << "network_service " << NETWORK_SERVICE_VERSION
              << " started: socket=" << options.socket_path
              << " eth=" << options.eth_iface
              << " wifi=" << options.wifi_iface
              << " config_dir=" << options.config_dir
              << " event_dir=" << options.event_dir
              << " mode=explicit_apply" << std::endl;

    server.run();
    network_service::NetworkIpcServer::set_wake_fd(-1);
    return 0;
}
