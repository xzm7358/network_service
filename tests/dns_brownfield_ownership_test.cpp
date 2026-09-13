#include "service/network_control_plane.h"

#include <iostream>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace {

bool expect(bool condition, const char *message) {
    if (condition) return true;
    std::cerr << "dns_brownfield_ownership_test: " << message << std::endl;
    return false;
}

struct FakePlatform {
    std::map<std::string, network_service::DhcpLeaseFact> leases;
    network_service::NetworkSnapshot snapshot;
    std::vector<std::string> dns_writes;
    int dns_clears = 0;

    network_service::NetworkControlPlaneOps ops() {
        network_service::NetworkControlPlaneOps out;
        out.start_dhcp = [](const std::string &, std::string &) { return true; };
        out.stop_dhcp = [](const std::string &) {};
        out.read_lease = [this](const std::string &iface,
                                network_service::DhcpLeaseFact &fact,
                                bool &exists,
                                std::string &) {
            const auto it = leases.find(iface);
            exists = it != leases.end();
            if (exists) fact = it->second;
            return true;
        };
        out.clear_lease = [this](const std::string &iface) { leases.erase(iface); };
        out.apply_ipv4 = [this](const std::string &iface,
                                const std::string &ip,
                                const std::string &mask,
                                std::string &) {
            auto &target = iface == "eth0" ? snapshot.eth : snapshot.wifi;
            target.iface = iface;
            target.exists = true;
            target.has_ip = true;
            target.ip4 = ip;
            target.netmask4 = mask;
            return true;
        };
        out.clear_ipv4 = [this](const std::string &iface, std::string &) {
            auto &target = iface == "eth0" ? snapshot.eth : snapshot.wifi;
            target.has_ip = false;
            target.ip4.clear();
            target.netmask4.clear();
            return true;
        };
        out.set_default_route = [this](const std::string &iface,
                                       const std::string &gateway,
                                       int metric,
                                       std::string &) {
            auto &target = iface == "eth0" ? snapshot.eth : snapshot.wifi;
            target.iface = iface;
            target.exists = true;
            target.has_default_route = true;
            target.gateway4 = gateway;
            target.route_metric = metric;
            return true;
        };
        out.clear_default_route = [this](const std::string &iface,
                                         const std::string &,
                                         int,
                                         std::string &) {
            auto &target = iface == "eth0" ? snapshot.eth : snapshot.wifi;
            target.has_default_route = false;
            target.gateway4.clear();
            target.route_metric = -1;
            return true;
        };
        out.set_dns = [this](const std::string &dns, std::string &) {
            dns_writes.push_back(dns);
            snapshot.dns4 = dns;
            snapshot.dns_available = !dns.empty();
            snapshot.dns_managed_by_network_service = true;
            return true;
        };
        out.clear_dns = [this](std::string &) {
            ++dns_clears;
            snapshot.dns4.clear();
            snapshot.dns_available = false;
            snapshot.dns_managed_by_network_service = false;
            return true;
        };
        out.snapshot = [this]() { return snapshot; };
        return out;
    }
};

network_service::DhcpLeaseFact wifi_lease(const std::string &dns) {
    network_service::DhcpLeaseFact fact;
    fact.event = network_service::DhcpLeaseEvent::Bound;
    fact.iface = "wlan0";
    fact.generation = "brownfield";
    fact.ip4 = "10.0.0.20";
    fact.netmask4 = "255.255.255.0";
    fact.gateway4 = "10.0.0.1";
    fact.dns4 = dns;
    return fact;
}

} // namespace

int main() {
    using network_service::NetworkControlPlane;

    bool ok = true;
    std::string error;

    {
        FakePlatform fake;
        fake.snapshot.wifi.iface = "wlan0";
        fake.snapshot.wifi.exists = true;
        fake.snapshot.dns4 = "8.8.8.8";
        fake.snapshot.dns_available = true;
        fake.snapshot.dns_managed_by_network_service = true;
        fake.leases["wlan0"] = wifi_lease("8.8.8.8");

        NetworkControlPlane plane("eth0", "wlan0", fake.ops());
        ok = expect(plane.adopt_dhcp("wlan0", error),
                    "failed to adopt DHCP lifecycle") && ok;

        bool changed = false;
        ok = expect(plane.reconcile(changed, error),
                    "marker-owned resolver reconcile failed") && ok;
        ok = expect(changed, "first reconcile must publish ownership initialization") && ok;
        ok = expect(fake.dns_writes.empty(),
                    "matching marker-owned resolver must be adopted without rewrite") && ok;
        ok = expect(fake.dns_clears == 0,
                    "matching marker-owned resolver must not be cleared") && ok;
    }

    {
        FakePlatform fake;
        fake.snapshot.dns4 = "9.9.9.9";
        fake.snapshot.dns_available = true;
        fake.snapshot.dns_managed_by_network_service = true;

        NetworkControlPlane plane("eth0", "wlan0", fake.ops());
        bool changed = false;
        ok = expect(plane.reconcile(changed, error),
                    "orphaned marker resolver reconcile failed") && ok;
        ok = expect(changed, "DNS ownership initialization must make first reconcile observable") && ok;
        ok = expect(fake.dns_clears == 1,
                    "old NetworkService resolver with no owned route must be relinquished") && ok;
        ok = expect(fake.dns_writes.empty(),
                    "orphaned resolver cleanup must not install new DNS") && ok;
    }

    {
        FakePlatform fake;
        fake.snapshot.dns4 = "4.4.4.4";
        fake.snapshot.dns_available = true;
        fake.snapshot.dns_managed_by_network_service = false;

        NetworkControlPlane plane("eth0", "wlan0", fake.ops());
        bool changed = false;
        ok = expect(plane.reconcile(changed, error),
                    "external resolver initialization failed") && ok;
        ok = expect(changed, "first DNS ownership initialization must run") && ok;
        ok = expect(fake.dns_clears == 0 && fake.dns_writes.empty(),
                    "unmarked external resolver must remain untouched") && ok;

        changed = true;
        ok = expect(plane.reconcile(changed, error),
                    "steady external resolver reconcile failed") && ok;
        ok = expect(!changed,
                    "DNS ownership initialization must run exactly once without other changes") && ok;
    }

    return ok ? 0 : 1;
}
