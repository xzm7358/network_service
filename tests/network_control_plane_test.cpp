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

    // Brownfield adoption must be non-mutating itself. Once imported, the normal
    // reconciliation path consumes the already-existing lease and converges the
    // owned IPv4/route/DNS state without starting a second DHCP lifecycle.
    FakePlatform adoption;
    adoption.leases["wlan0"] = lease("wlan0", "10.0.0.60", "10.0.0.1", "9.9.9.9");
    NetworkControlPlane adoption_plane("eth0", "wlan0", adoption.ops());
    adoption.reset_observations();
    ok = expect(adoption_plane.adopt_dhcp("wlan0", error),
                "brownfield DHCP adoption failed") && ok;
    ok = expect(adoption.ip_applies.empty() && adoption.routes.empty() &&
                adoption.dns_writes.empty(),
                "adoption must not mutate network state before reconcile") && ok;
    bool adoption_changed = false;
    ok = expect(adoption_plane.reconcile(adoption_changed, error),
                "adopted DHCP reconcile failed") && ok;
    ok = expect(adoption_changed,
                "existing lease was not imported as a reconciliation change") && ok;
    ok = expect(!adoption.ip_applies.empty() &&
                adoption.ip_applies.back() == "wlan0=10.0.0.60",
                "adopted lease did not converge IPv4") && ok;
    ok = expect(!adoption.routes.empty() &&
                std::get<0>(adoption.routes.back()) == "wlan0",
                "adopted lease did not converge default route") && ok;
    ok = expect(!adoption.dns_writes.empty() && adoption.dns_writes.back() == "9.9.9.9",
                "adopted lease did not converge DNS") && ok;

    // Netlink/external observation repair: once a lease is active, missing owned
    // IP/default-route state must be restored even though the lease fingerprint
    // itself has not changed.
    FakePlatform repair;
    NetworkControlPlane repair_plane("eth0", "wlan0", repair.ops());
    ok = expect(repair_plane.start_dhcp("wlan0", error), "repair wifi start failed") && ok;
    repair.leases["wlan0"] = lease("wlan0", "10.0.0.70", "10.0.0.1", "8.8.8.8");
    ok = expect(repair_plane.reconcile(error), "repair initial lease reconcile failed") && ok;

    repair.snapshot.wifi.iface = "wlan0";
    repair.snapshot.wifi.exists = true;
    repair.snapshot.wifi.has_ip = true;
    repair.snapshot.wifi.ip4 = "10.0.0.70";
    repair.snapshot.wifi.netmask4 = "255.255.255.0";
    repair.snapshot.wifi.has_default_route = true;
    repair.snapshot.wifi.gateway4 = "10.0.0.1";
    repair.snapshot.wifi.route_metric = 20;
    repair.snapshot.dns4 = "8.8.8.8";
    repair.snapshot.dns_available = true;
    repair.reset_observations();
    ok = expect(repair_plane.refresh_external_state(error),
                "healthy owned-state refresh failed") && ok;
    ok = expect(repair.ip_applies.empty() && repair.routes.empty(),
                "healthy owned state must not be re-applied") && ok;

    repair.snapshot.wifi.has_default_route = false;
    repair.reset_observations();
    ok = expect(repair_plane.refresh_external_state(error),
                "missing-route repair failed") && ok;
    ok = expect(repair.routes.size() == 1 &&
                std::get<0>(repair.routes.back()) == "wlan0" &&
                std::get<1>(repair.routes.back()) == "10.0.0.1" &&
                std::get<2>(repair.routes.back()) == 20,
                "missing owned Wi-Fi default route was not restored") && ok;

    repair.snapshot.wifi.has_default_route = true;
    repair.snapshot.wifi.gateway4 = "10.0.0.1";
    repair.snapshot.wifi.route_metric = 20;
    repair.snapshot.wifi.has_ip = false;
    repair.snapshot.wifi.ip4.clear();
    repair.snapshot.wifi.netmask4.clear();
    repair.reset_observations();
    ok = expect(repair_plane.refresh_external_state(error),
                "missing-IP repair failed") && ok;
    ok = expect(repair.ip_applies.size() == 1 &&
                repair.ip_applies.back() == "wlan0=10.0.0.70",
                "missing owned Wi-Fi IPv4 was not restored") && ok;

    // Until route identity is implemented, a different live route on the same
    // interface is preserved rather than destructively replaced.
    repair.snapshot.wifi.has_ip = true;
    repair.snapshot.wifi.ip4 = "10.0.0.70";
    repair.snapshot.wifi.netmask4 = "255.255.255.0";
    repair.snapshot.wifi.has_default_route = true;
    repair.snapshot.wifi.gateway4 = "192.0.2.1";
    repair.snapshot.wifi.route_metric = 99;
    repair.reset_observations();
    ok = expect(repair_plane.refresh_external_state(error),
                "foreign-route preservation refresh failed") && ok;
    ok = expect(repair.routes.empty() && repair.route_clears.empty(),
                "F-04 must not replace/delete a different live route") && ok;

    // DELLINK/driver disappearance is not a Service repair target. Avoid a 250ms
    // mutation loop against an interface that no longer exists.
    repair.snapshot.wifi.exists = false;
    repair.snapshot.wifi.has_ip = false;
    repair.snapshot.wifi.has_default_route = false;
    repair.reset_observations();
    ok = expect(repair_plane.refresh_external_state(error),
                "missing-interface refresh failed") && ok;
    ok = expect(repair.ip_applies.empty() && repair.routes.empty(),
                "missing interface must not trigger owned-state mutation") && ok;

    // Static Ethernet owns an IPv4 desired fact too; preserve enough desired
    // state to restore an externally removed address.
    FakePlatform static_repair;
    NetworkControlPlane static_plane("eth0", "wlan0", static_repair.ops());
    ok = expect(static_plane.apply_ethernet_static("192.168.50.20",
                                                   "255.255.255.0",
                                                   "192.168.50.1",
                                                   "1.1.1.1",
                                                   10,
                                                   error),
                "static repair setup failed") && ok;
    static_repair.snapshot.eth.iface = "eth0";
    static_repair.snapshot.eth.exists = true;
    static_repair.snapshot.eth.has_ip = false;
    static_repair.snapshot.eth.has_default_route = true;
    static_repair.snapshot.eth.gateway4 = "192.168.50.1";
    static_repair.snapshot.eth.route_metric = 10;
    static_repair.snapshot.dns4 = "1.1.1.1";
    static_repair.snapshot.dns_available = true;
    static_repair.reset_observations();
    ok = expect(static_plane.refresh_external_state(error),
                "static IPv4 repair failed") && ok;
    ok = expect(static_repair.ip_applies.size() == 1 &&
                static_repair.ip_applies.back() == "eth0=192.168.50.20",
                "missing owned static IPv4 was not restored") && ok;

    return ok ? 0 : 1;
}