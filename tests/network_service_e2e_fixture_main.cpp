#include "ipc/network_ipc_server.h"
#include "service/network_daemon.h"

#include <csignal>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>
#include <utility>

namespace {

struct Options {
    std::string socket_path;
    std::string state_file;
    std::string config_dir = "/tmp";
    std::string event_dir = "/tmp/network-service-e2e-no-wpa";
};

void handle_signal(int) {
    const char wake = 'x';
    const int fd = network_service::NetworkIpcServer::wake_fd();
    if (fd >= 0) {
        (void)!::write(fd, &wake, sizeof(wake));
    }
}

void print_usage(const char *argv0) {
    std::cerr << "Usage: " << argv0
              << " --socket PATH --state-file PATH"
              << " [--config-dir DIR] [--event-dir DIR]\n";
}

bool parse_args(int argc, char **argv, Options &options) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto require_value = [&](std::string &out) -> bool {
            if (i + 1 >= argc) return false;
            out = argv[++i];
            return !out.empty();
        };

        if (arg == "--socket") {
            if (!require_value(options.socket_path)) return false;
        } else if (arg == "--state-file") {
            if (!require_value(options.state_file)) return false;
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
    return !options.socket_path.empty() && !options.state_file.empty();
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

network_service::NetworkSnapshot connected_snapshot() {
    network_service::NetworkSnapshot snapshot = disconnected_snapshot();
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

network_service::NetworkSnapshot snapshot_from_file(const std::string &path) {
    std::ifstream input(path);
    std::string state;
    if (input) input >> state;
    if (state == "connected") return connected_snapshot();
    return disconnected_snapshot();
}

} // namespace

int main(int argc, char **argv) {
    Options options;
    if (!parse_args(argc, argv, options)) {
        print_usage(argv[0]);
        return 1;
    }

    std::signal(SIGTERM, handle_signal);
    std::signal(SIGINT, handle_signal);
#ifdef SIGPIPE
    std::signal(SIGPIPE, SIG_IGN);
#endif

    network_service::NetworkDaemon daemon(
        "eth0",
        "wlan0",
        options.config_dir,
        options.event_dir,
        [&options]() { return snapshot_from_file(options.state_file); });
    network_service::NetworkIpcServer server(daemon);
    if (server.wake_write_fd() < 0) {
        std::cerr << "network_service_e2e_fixture: failed to create wake pipe" << std::endl;
        return 2;
    }
    network_service::NetworkIpcServer::set_wake_fd(server.wake_write_fd());
    if (!server.listen(options.socket_path)) {
        network_service::NetworkIpcServer::set_wake_fd(-1);
        return 2;
    }

    std::cout << "NETWORK_SERVICE_E2E_FIXTURE_READY socket=" << options.socket_path
              << " state_file=" << options.state_file << std::endl;
    std::cout.flush();

    server.run();
    network_service::NetworkIpcServer::set_wake_fd(-1);
    return 0;
}
