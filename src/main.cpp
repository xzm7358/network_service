#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <unistd.h>

#ifndef NETWORK_SERVICE_VERSION
#define NETWORK_SERVICE_VERSION "0.1.0"
#endif

#include "ipc/network_ipc_server.h"
#include "network_service_protocol.h"
#include "service/network_daemon.h"
#include "service/ethernet_startup.h"

namespace {

constexpr const char *kProcessLockPath = "/tmp/network_service.lock";

class ProcessSingletonLock {
public:
    ~ProcessSingletonLock() {
        if (fd_ >= 0) close(fd_);
    }

    ProcessSingletonLock(const ProcessSingletonLock &) = delete;
    ProcessSingletonLock &operator=(const ProcessSingletonLock &) = delete;
    ProcessSingletonLock() = default;

    bool acquire(std::string &error) {
        error.clear();
        if (fd_ >= 0) return true;

        const int fd = open(kProcessLockPath, O_CREAT | O_RDWR, 0644);
        if (fd < 0) {
            error = std::string("failed to open singleton lock: ") + std::strerror(errno);
            return false;
        }

        struct flock lock{};
        lock.l_type = F_WRLCK;
        lock.l_whence = SEEK_SET;
        lock.l_start = 0;
        lock.l_len = 0;
        if (fcntl(fd, F_SETLK, &lock) != 0) {
            const int saved_errno = errno;
            close(fd);
            if (saved_errno == EACCES || saved_errno == EAGAIN) {
                error = "another network_service instance owns the process lock";
            } else {
                error = std::string("failed to acquire singleton lock: ") +
                        std::strerror(saved_errno);
            }
            return false;
        }

        const int descriptor_flags = fcntl(fd, F_GETFD, 0);
        if (descriptor_flags < 0 ||
            fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC) != 0) {
            const int saved_errno = errno;
            close(fd);
            error = std::string("failed to protect singleton lock across exec: ") +
                    std::strerror(saved_errno);
            return false;
        }

        const std::string owner = std::to_string(static_cast<long>(getpid())) + "\n";
        (void)ftruncate(fd, 0);
        (void)lseek(fd, 0, SEEK_SET);
        (void)write(fd, owner.data(), owner.size());
        fd_ = fd;
        return true;
    }

private:
    int fd_ = -1;
};

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
    std::string config_dir = "/data";
    std::string event_dir = "/var/run/wpa_supplicant";
    std::string capability;
    bool validate_ethernet_config = false;
};

void print_usage(const char *argv0) {
    std::cerr << "Usage: " << argv0
              << " [--socket PATH] [--eth IFACE] [--wifi IFACE]"
              << " [--config-dir DIR] [--event-dir DIR]"
              << " [--check-capability NAME] [--validate-ethernet-config]\n";
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
        } else if (arg == "--check-capability") {
            if (!require_value(options.capability)) return false;
        } else if (arg == "--validate-ethernet-config") {
            options.validate_ethernet_config = true;
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
    if (!options.capability.empty()) {
        if (options.capability == "ethernet-json-v1") {
            std::cout << options.capability << std::endl;
            return 0;
        }
        std::cerr << "Unsupported capability: " << options.capability << std::endl;
        return 1;
    }
    if (options.validate_ethernet_config) {
        std::string method;
        std::string error;
        if (!network_service::validate_ethernet_startup_config(
                options.config_dir, options.eth_iface, method, error)) {
            std::cerr << "Invalid Ethernet configuration: " << error << std::endl;
            return 1;
        }
        std::cout << method << std::endl;
        return 0;
    }

    ProcessSingletonLock process_lock;
    std::string lock_error;
    if (!process_lock.acquire(lock_error)) {
        std::cerr << "network_service: PROCESS_OWNERSHIP_REJECTED error="
                  << lock_error << std::endl;
        return 3;
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
