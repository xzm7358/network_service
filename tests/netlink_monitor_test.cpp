#include "platform/netlink_monitor.h"

#include <iostream>
#include <string>

#ifdef __linux__
#include <linux/if_addr.h>
#include <linux/if_link.h>
#include <linux/rtnetlink.h>
#include <sys/socket.h>
#endif

namespace {

bool expect(bool condition, const char *message) {
    if (condition) return true;
    std::cerr << "netlink_monitor_test: " << message << std::endl;
    return false;
}

} // namespace

int main() {
    bool ok = true;

#ifdef __linux__
    ifinfomsg link{};
    ok = expect(network_service::NetlinkMonitor::affects_network_state(
                    RTM_NEWLINK, &link, sizeof(link)),
                "RTM_NEWLINK must affect network state") && ok;
    ok = expect(network_service::NetlinkMonitor::affects_network_state(
                    RTM_DELLINK, &link, sizeof(link)),
                "RTM_DELLINK must affect network state") && ok;

    ifaddrmsg ipv4_addr{};
    ipv4_addr.ifa_family = AF_INET;
    ok = expect(network_service::NetlinkMonitor::affects_network_state(
                    RTM_NEWADDR, &ipv4_addr, sizeof(ipv4_addr)),
                "IPv4 RTM_NEWADDR must affect network state") && ok;

    ifaddrmsg ipv6_addr{};
    ipv6_addr.ifa_family = AF_INET6;
    ok = expect(!network_service::NetlinkMonitor::affects_network_state(
                    RTM_NEWADDR, &ipv6_addr, sizeof(ipv6_addr)),
                "IPv6 address events are outside the v1 truth model") && ok;

    rtmsg default_route{};
    default_route.rtm_family = AF_INET;
    default_route.rtm_dst_len = 0;
    ok = expect(network_service::NetlinkMonitor::affects_network_state(
                    RTM_NEWROUTE, &default_route, sizeof(default_route)),
                "IPv4 default route must affect network state") && ok;

    rtmsg scoped_route{};
    scoped_route.rtm_family = AF_INET;
    scoped_route.rtm_dst_len = 24;
    ok = expect(!network_service::NetlinkMonitor::affects_network_state(
                    RTM_NEWROUTE, &scoped_route, sizeof(scoped_route)),
                "non-default route must not trigger v1 network-state observation") && ok;

    ok = expect(!network_service::NetlinkMonitor::affects_network_state(
                    RTM_NEWLINK, nullptr, 0),
                "truncated link payload must be rejected") && ok;

    network_service::NetlinkMonitor monitor;
    std::string error;
    ok = expect(monitor.fd() == -1, "fresh monitor must not own an fd") && ok;
    ok = expect(monitor.open(error), "Netlink open failed on Linux host") && ok;
    if (monitor.fd() >= 0) {
        bool changed = true;
        error.clear();
        ok = expect(monitor.drain(changed, error), "empty nonblocking drain failed") && ok;
        ok = expect(error.empty(), "empty drain reported an error") && ok;
        monitor.close();
        ok = expect(monitor.fd() == -1, "close must release Netlink fd") && ok;
        monitor.close();
        ok = expect(monitor.fd() == -1, "close must be idempotent") && ok;
    }
#else
    network_service::NetlinkMonitor monitor;
    std::string error;
    ok = expect(monitor.open(error), "non-Linux monitor should degrade cleanly") && ok;
    ok = expect(monitor.fd() == -1, "non-Linux monitor must expose fd=-1") && ok;
#endif

    return ok ? 0 : 1;
}
