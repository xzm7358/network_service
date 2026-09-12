#include "service/network_control_plane.h"

#include <iostream>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace {

bool expect(bool condition, const char *message) {
    if (condition) return true;
    std::cerr << "network_control_plane_test: " << message << std::endl;
    return false;
}

struct FakePlatform {
    std::map<std::string, network_service::DhcpLeaseFact> leases;
    std::vector<std::string> ip_applies;
    std::vector<std::string> ip_clears;
    std::vector<std::tuple<std::string, std::string, int>> routes;
    std::vector<std::string> route_clears;
    std::vector<std::string> dns_writes;
    int dns_clears = 0;
    network_service::NetworkSnapshot snapshot;

    network_service::NetworkControlPlaneOps ops() {
        network_service::NetworkControlPlaneOps out;
        out.start_dhcp = [](const std::string &, std::string &) { return true; };
        out.stop_dhcp = [](const std::string &) {};
        out.read_lease = [this](const std::string &iface,
                                network_service::DhcpLeaseFact &fact,
                                bool &exists,
                                std::string &) {
            auto it = leases.find(iface);
            exists = it != leases.end();
            if (exists) fact = it->second;
            return true;
        };
        out.clear_lease = [this](const std::string &iface) { leases.erase(iface); };
        out.apply_ipv4 = [this](const std::string &iface,
                                const std::string &ip,
                                const std::string &,
                                std::string &) {
            ip_applies.push_back(iface + "=" + ip);
            return true;
        };
        out.clear_ipv4 = [this](const std::string &iface, std::string &) {
            ip_clears.push_back(iface);
            return true;
        };
        out.set_default_route = [this](const std::string &iface,
                                       const std::string &gateway,
                                       int metric,
                                       std::string &) {
            routes.emplace_back(iface, gateway, metric);
            return true;
        };
        out.clear_default_route = [this](const std::string &iface) {
            route_clears.push_back(iface);
        };
        out.set_dns = [this](const std::string &dns, std::string &) {
            dns_writes.push_back(dns);
            snapshot.dns4 = dns;
            snapshot.dns_available = !dns.empty();
            return true;
        };
        out.clear_dns = [this](std::string &) {
            ++dns_clears;
            snapshot.dns4.clear();
            snapshot.dns_available = false;
            return true;
        };
        out.snapshot = [this]() { return snapshot; };
        return out;
    }

    void reset_observations() {
        ip_applies.clear();
        ip_clears.clear();
        routes.clear();
        route_clears.clear();
        dns_writes.clear();
        dns_clears = 0;
    }
};

network_service::DhcpLeaseFact lease(const std::string &iface,
                                     const std::string &ip,
                                     const std::string &gateway,
                                     const std::string &dns) {
    network_service::DhcpLeaseFact out;
    out.event = network_service::DhcpLeaseEvent::Bound;
    out.iface = iface;
    out.generation = "test";
    out.ip4 = ip;
    out.netmask4 = "255.255.255.0";
    out.gateway4 = gateway;
    out.dns4 = dns;
    return out;
}

} // namespace

int main() {
    using network_service::NetworkControlPlane;
    using network_service::RoutePolicy;

    bool ok = true;
    FakePlatform fake;
    NetworkControlPlane plane("eth0", "wlan0", fake.ops());
    std::string error;

    ok = expect(plane.start_dhcp("eth0", error), "eth DHCP start failed") && ok;
    ok = expect(plane.start_dhcp("wlan0", error), "wifi DHCP start failed") && ok;
    fake.reset_observations();

    fake.leases["eth0"] = lease("eth0", "192.168.1.20", "192.168.1.1", "1.1.1.1");
    fake.leases["wlan0"] = lease("wlan0", "10.0.0.20", "10.0.0.1", "8.8.8.8");

    ok = expect(plane.reconcile(error), "EthernetPreferred reconcile failed") && ok;
    ok = expect(fake.ip_applies.size() == 2, "both leases must configure IP") && ok;
    ok = expect(fake.routes.size() == 2, "both default routes must be installed") && ok;
    if (fake.routes.size() == 2) {
        ok = expect(std::get<0>(fake.routes[0]) == "eth0" &&
                    std::get<2>(fake.routes[0]) == 10,
                    "EthernetPreferred must assign eth metric 10") && ok;
        ok = expect(std::get<0>(fake.routes[1]) == "wlan0" &&
                    std::get<2>(fake.routes[1]) == 20,
                    "EthernetPreferred must assign wifi metric 20") && ok;
    }
    ok = expect(!fake.dns_writes.empty() && fake.dns_writes.back() == "1.1.1.1",
                "EthernetPreferred DNS must follow Ethernet") && ok;

    const std::size_t ip_count = fake.ip_applies.size();
    const std::size_t route_count = fake.routes.size();
    const std::size_t dns_count = fake.dns_writes.size();
    ok = expect(plane.reconcile(error), "duplicate reconcile failed") && ok;
    ok = expect(fake.ip_applies.size() == ip_count &&
                fake.routes.size() == route_count &&
                fake.dns_writes.size() == dns_count,
                "unchanged lease fact must be idempotent") && ok;

    fake.reset_observations();
    ok = expect(plane.set_route_policy(RoutePolicy::WifiPreferred, error),
                "WifiPreferred transition failed") && ok;
    ok = expect(fake.routes.size() == 2, "WifiPreferred must reapply both routes") && ok;
    if (fake.routes.size() == 2) {
        ok = expect(std::get<2>(fake.routes[0]) == 20,
                    "WifiPreferred must demote Ethernet") && ok;
        ok = expect(std::get<2>(fake.routes[1]) == 10,
                    "WifiPreferred must promote Wi-Fi") && ok;
    }
    ok = expect(!fake.dns_writes.empty() && fake.dns_writes.back() == "8.8.8.8",
                "WifiPreferred DNS must follow Wi-Fi") && ok;

    fake.reset_observations();
    ok = expect(plane.set_route_policy(RoutePolicy::WifiOnly, error),
                "WifiOnly transition failed") && ok;
    ok = expect(!fake.route_clears.empty() && fake.route_clears.back() == "eth0",
                "WifiOnly must remove managed Ethernet default route") && ok;
    ok = expect(!fake.routes.empty() && std::get<0>(fake.routes.back()) == "wlan0" &&
                std::get<2>(fake.routes.back()) == 10,
                "WifiOnly must keep Wi-Fi as primary route") && ok;

    FakePlatform failover;
    NetworkControlPlane failover_plane("eth0", "wlan0", failover.ops());
    ok = expect(failover_plane.start_dhcp("eth0", error), "failover eth start failed") && ok;
    ok = expect(failover_plane.start_dhcp("wlan0", error), "failover wifi start failed") && ok;
    failover.reset_observations();
    failover.leases["eth0"] = lease("eth0", "192.168.1.30", "192.168.1.1", "1.1.1.1");
    failover.leases["wlan0"] = lease("wlan0", "10.0.0.30", "10.0.0.1", "8.8.8.8");
    ok = expect(failover_plane.reconcile(error), "failover initial reconcile failed") && ok;

    network_service::DhcpLeaseFact deconfig;
    deconfig.event = network_service::DhcpLeaseEvent::Deconfig;
    deconfig.iface = "eth0";
    deconfig.generation = "test";
    failover.leases["eth0"] = deconfig;
    failover.reset_observations();
    ok = expect(failover_plane.reconcile(error), "eth deconfig reconcile failed") && ok;
    ok = expect(!failover.route_clears.empty() && failover.route_clears.back() == "eth0",
                "eth deconfig must remove only eth route") && ok;
    ok = expect(!failover.dns_writes.empty() && failover.dns_writes.back() == "8.8.8.8",
                "eth deconfig must fail DNS over to Wi-Fi") && ok;

    FakePlatform protected_eth;
    protected_eth.snapshot.eth.has_default_route = true;
    protected_eth.snapshot.dns4 = "4.4.4.4";
    protected_eth.snapshot.dns_available = true;
    NetworkControlPlane protected_plane("eth0", "wlan0", protected_eth.ops());
    ok = expect(protected_plane.start_dhcp("wlan0", error), "protected wifi start failed") && ok;
    protected_eth.reset_observations();
    protected_eth.leases["wlan0"] = lease("wlan0", "10.0.0.40", "10.0.0.1", "8.8.4.4");
    ok = expect(protected_plane.reconcile(error), "protected eth reconcile failed") && ok;
    ok = expect(protected_eth.dns_writes.empty(),
                "unmanaged live Ethernet route must prevent Wi-Fi DNS overwrite") && ok;

    // Start with Wi-Fi as the only route, then let an externally-managed
    // Ethernet route recover. NetworkService must relinquish its DNS ownership
    // instead of leaving Wi-Fi DNS sticky under EthernetPreferred.
    FakePlatform recovery;
    NetworkControlPlane recovery_plane("eth0", "wlan0", recovery.ops());
    ok = expect(recovery_plane.start_dhcp("wlan0", error), "recovery wifi start failed") && ok;
    recovery.reset_observations();
    recovery.leases["wlan0"] = lease("wlan0", "10.0.0.50", "10.0.0.1", "8.8.8.8");
    ok = expect(recovery_plane.reconcile(error), "recovery wifi reconcile failed") && ok;
    ok = expect(!recovery.dns_writes.empty() && recovery.dns_writes.back() == "8.8.8.8",
                "Wi-Fi must own DNS while Ethernet route is absent") && ok;

    recovery.reset_observations();
    recovery.snapshot.eth.has_default_route = true;
    ok = expect(recovery_plane.reconcile(error), "external Ethernet recovery failed") && ok;
    ok = expect(recovery.dns_clears == 1,
                "external Ethernet recovery must relinquish managed Wi-Fi DNS") && ok;
    ok = expect(recovery.dns_writes.empty(),
                "external Ethernet recovery must not install another managed DNS") && ok;

    // Once the external owner has installed its own DNS, another reconcile must
    // not clear or overwrite it.
    recovery.reset_observations();
    recovery.snapshot.dns4 = "4.4.4.4";
    recovery.snapshot.dns_available = true;
    ok = expect(recovery_plane.reconcile(error), "external DNS preservation failed") && ok;
    ok = expect(recovery.dns_clears == 0 && recovery.dns_writes.empty(),
                "external DNS must remain untouched while Ethernet owns the route") && ok;

    // If external Ethernet disappears again, Wi-Fi becomes primary and its DNS
    // must be actively restored rather than skipped because of stale cache.
    recovery.reset_observations();
    recovery.snapshot.eth.has_default_route = false;
    ok = expect(recovery_plane.reconcile(error), "Wi-Fi DNS reacquire failed") && ok;
    ok = expect(!recovery.dns_writes.empty() && recovery.dns_writes.back() == "8.8.8.8",
                "Wi-Fi DNS must be restored after external Ethernet disappears") && ok;

    return ok ? 0 : 1;
}
