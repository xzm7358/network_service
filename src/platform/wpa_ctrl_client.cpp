#include "platform/wpa_ctrl_client.h"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <poll.h>
#include <sstream>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace network_service {
namespace {

std::atomic<unsigned long> g_ctrl_sequence{0};

std::string make_local_path() {
    std::ostringstream os;
    os << "/tmp/network_service_ctrl_" << static_cast<unsigned long>(getpid())
       << '_' << g_ctrl_sequence.fetch_add(1, std::memory_order_relaxed) << ".sock";
    return os.str();
}

bool fill_unix_address(const std::string &path, sockaddr_un &addr, std::string &error) {
    if (path.empty() || path.size() >= sizeof(addr.sun_path)) {
        error = "wpa ctrl socket path is invalid or too long";
        return false;
    }
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());
    return true;
}

} // namespace

WpaCtrlClient::WpaCtrlClient(std::string ctrl_path)
    : ctrl_path_(std::move(ctrl_path)) {}

WpaCtrlClient::~WpaCtrlClient() {
    close();
}

bool WpaCtrlClient::open(std::string &error) {
    error.clear();
    if (fd_ >= 0) return true;

    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) {
        error = std::string("wpa ctrl socket failed: ") + std::strerror(errno);
        return false;
    }

    local_path_ = make_local_path();
    unlink(local_path_.c_str());

    sockaddr_un local{};
    if (!fill_unix_address(local_path_, local, error) ||
        bind(fd, reinterpret_cast<sockaddr *>(&local), sizeof(local)) != 0) {
        if (error.empty()) {
            error = std::string("wpa ctrl bind failed: ") + std::strerror(errno);
        }
        ::close(fd);
        unlink(local_path_.c_str());
        local_path_.clear();
        return false;
    }

    sockaddr_un remote{};
    if (!fill_unix_address(ctrl_path_, remote, error) ||
        connect(fd, reinterpret_cast<sockaddr *>(&remote), sizeof(remote)) != 0) {
        if (error.empty()) {
            error = std::string("wpa ctrl connect failed: ") + std::strerror(errno);
        }
        ::close(fd);
        unlink(local_path_.c_str());
        local_path_.clear();
        return false;
    }

    fd_ = fd;
    return true;
}

void WpaCtrlClient::close() {
    if (fd_ >= 0) {
        if (attached_) detach();
        ::close(fd_);
        fd_ = -1;
    }
    if (!local_path_.empty()) {
        unlink(local_path_.c_str());
        local_path_.clear();
    }
    attached_ = false;
}

bool WpaCtrlClient::is_open() const {
    return fd_ >= 0;
}

bool WpaCtrlClient::request(const std::string &command,
                            std::string &reply,
                            std::string &error,
                            int timeout_ms) {
    reply.clear();
    error.clear();
    if (fd_ < 0 && !open(error)) return false;
    if (command.empty()) {
        error = "wpa ctrl command is empty";
        return false;
    }

    const ssize_t sent = send(fd_, command.data(), command.size(), 0);
    if (sent != static_cast<ssize_t>(command.size())) {
        error = std::string("wpa ctrl send failed: ") + std::strerror(errno);
        return false;
    }

    pollfd pfd{};
    pfd.fd = fd_;
    pfd.events = POLLIN;
    for (;;) {
        const int rc = poll(&pfd, 1, timeout_ms);
        if (rc > 0) break;
        if (rc == 0) {
            error = "wpa ctrl request timed out";
            return false;
        }
        if (errno == EINTR) continue;
        error = std::string("wpa ctrl poll failed: ") + std::strerror(errno);
        return false;
    }

    char buffer[4096];
    const ssize_t n = recv(fd_, buffer, sizeof(buffer), 0);
    if (n < 0) {
        error = std::string("wpa ctrl recv failed: ") + std::strerror(errno);
        return false;
    }
    reply.assign(buffer, static_cast<std::size_t>(n));
    return true;
}

bool WpaCtrlClient::attach(std::string &error) {
    if (attached_) return true;
    std::string reply;
    if (!request("ATTACH", reply, error)) return false;
    if (reply.find("OK") == std::string::npos) {
        error = "wpa ctrl ATTACH rejected: " + reply;
        return false;
    }
    attached_ = true;
    return true;
}

void WpaCtrlClient::detach() {
    if (!attached_ || fd_ < 0) return;
    std::string reply;
    std::string ignored;
    (void)request("DETACH", reply, ignored, 500);
    attached_ = false;
}

int WpaCtrlClient::fd() const {
    return fd_;
}

bool WpaCtrlClient::receive(std::string &message, std::string &error) {
    message.clear();
    error.clear();
    if (fd_ < 0) {
        error = "wpa ctrl socket is not open";
        return false;
    }
    char buffer[4096];
    const ssize_t n = recv(fd_, buffer, sizeof(buffer), 0);
    if (n <= 0) {
        error = n == 0 ? "wpa ctrl peer closed" :
                         std::string("wpa ctrl recv failed: ") + std::strerror(errno);
        return false;
    }
    message.assign(buffer, static_cast<std::size_t>(n));
    return true;
}

std::string wpa_ctrl_path_for(const std::string &iface, const std::string &ctrl_dir) {
    std::string base = ctrl_dir.empty() ? "/var/run/wpa_supplicant" : ctrl_dir;
    if (!base.empty() && base.back() == '/') base.pop_back();
    return base + "/" + iface;
}

} // namespace network_service
