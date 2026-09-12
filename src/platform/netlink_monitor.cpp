#include "platform/netlink_monitor.h"

#include <cerrno>
#include <cstring>

#ifdef __linux__
#include <fcntl.h>
#include <linux/if_addr.h>
#include <linux/if_link.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace network_service {

NetlinkMonitor::~NetlinkMonitor() {
    close();
}

bool NetlinkMonitor::open(std::string &error) {
    error.clear();
#ifdef __linux__
    if (fd_ >= 0) return true;

    const int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    if (fd < 0) {
        error = std::string("netlink socket failed: ") + std::strerror(errno);
        return false;
    }

    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        error = std::string("netlink nonblocking setup failed: ") + std::strerror(errno);
        ::close(fd);
        return false;
    }

    sockaddr_nl addr{};
    addr.nl_family = AF_NETLINK;
    addr.nl_pid = 0;
    addr.nl_groups = RTMGRP_LINK | RTMGRP_IPV4_IFADDR | RTMGRP_IPV4_ROUTE;
    if (bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        error = std::string("netlink bind failed: ") + std::strerror(errno);
        ::close(fd);
        return false;
    }

    fd_ = fd;
#else
    (void)error;
#endif
    return true;
}

void NetlinkMonitor::close() {
#ifdef __linux__
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
#else
    fd_ = -1;
#endif
}

int NetlinkMonitor::fd() const {
    return fd_;
}

bool NetlinkMonitor::affects_network_state(std::uint16_t message_type,
                                           const void *payload,
                                           std::size_t payload_size) {
#ifdef __linux__
    switch (message_type) {
    case RTM_NEWLINK:
    case RTM_DELLINK:
        return payload != nullptr && payload_size >= sizeof(ifinfomsg);

    case RTM_NEWADDR:
    case RTM_DELADDR:
        if (payload == nullptr || payload_size < sizeof(ifaddrmsg)) return false;
        return static_cast<const ifaddrmsg *>(payload)->ifa_family == AF_INET;

    case RTM_NEWROUTE:
    case RTM_DELROUTE:
        if (payload == nullptr || payload_size < sizeof(rtmsg)) return false;
        {
            const auto *route = static_cast<const rtmsg *>(payload);
            return route->rtm_family == AF_INET && route->rtm_dst_len == 0;
        }

    default:
        return false;
    }
#else
    (void)message_type;
    (void)payload;
    (void)payload_size;
    return false;
#endif
}

bool NetlinkMonitor::drain(bool &changed, std::string &error) {
    changed = false;
    error.clear();
#ifdef __linux__
    if (fd_ < 0) return true;

    alignas(nlmsghdr) char buffer[16 * 1024];
    for (;;) {
        const ssize_t n = recv(fd_, buffer, sizeof(buffer), MSG_DONTWAIT);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
            if (errno == ENOBUFS) {
                // Kernel dropped one or more notifications. Force an immediate
                // authoritative snapshot; low-frequency reconciliation remains
                // the final safety net.
                changed = true;
                return true;
            }
            error = std::string("netlink recv failed: ") + std::strerror(errno);
            close();
            return false;
        }
        if (n == 0) return true;

        int remaining = static_cast<int>(n);
        for (nlmsghdr *hdr = reinterpret_cast<nlmsghdr *>(buffer);
             NLMSG_OK(hdr, remaining);
             hdr = NLMSG_NEXT(hdr, remaining)) {
            if (hdr->nlmsg_type == NLMSG_DONE) continue;
            if (hdr->nlmsg_type == NLMSG_ERROR) {
                if (hdr->nlmsg_len >= NLMSG_LENGTH(sizeof(nlmsgerr))) {
                    const auto *nlerr =
                        reinterpret_cast<const nlmsgerr *>(NLMSG_DATA(hdr));
                    if (nlerr->error != 0) {
                        error = "netlink reported error " +
                                std::to_string(-nlerr->error);
                        close();
                        return false;
                    }
                }
                continue;
            }

            const std::size_t payload_size =
                hdr->nlmsg_len >= NLMSG_HDRLEN ? hdr->nlmsg_len - NLMSG_HDRLEN : 0;
            if (affects_network_state(static_cast<std::uint16_t>(hdr->nlmsg_type),
                                      NLMSG_DATA(hdr),
                                      payload_size)) {
                changed = true;
            }
        }

        // If a datagram did not fit our bounded stack buffer, some messages may
        // have been lost. Treat it like a change and force an authoritative read.
        if (n == static_cast<ssize_t>(sizeof(buffer))) changed = true;
    }
#else
    (void)error;
    return true;
#endif
}

} // namespace network_service
