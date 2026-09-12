#include "platform/dhcp_lease_store.h"

#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>

namespace {

bool expect(bool condition, const char *message) {
    if (condition) return true;
    std::cerr << "dhcp_lease_store_test: " << message << std::endl;
    return false;
}

void write_lease(const std::string &path,
                 const std::string &generation,
                 const std::string &event,
                 const std::string &dns) {
    std::ofstream out(path, std::ios::out | std::ios::trunc);
    out << "generation=" << generation << "\n"
        << "event=" << event << "\n"
        << "ip=192.168.50.20\n"
        << "subnet=255.255.255.0\n"
        << "router=192.168.50.1 192.168.50.2\n"
        << "dns=" << dns << " 8.8.8.8\n";
}

} // namespace

int main() {
    bool ok = true;
    const std::string iface = "utlease0";
    const std::string gen1 = "gen_1";
    const std::string gen2 = "gen_2";

    network_service::DhcpLeaseStore::clear(iface);

    network_service::DhcpLeaseFact fact;
    bool exists = true;
    std::string error;
    ok = expect(network_service::DhcpLeaseStore::read(iface, fact, exists, error),
                "missing lease should be a valid empty read") && ok;
    ok = expect(!exists, "missing generation should report exists=false") && ok;

    ok = expect(network_service::DhcpLeaseStore::activate_generation(iface, gen1, error),
                "failed to activate first generation") && ok;
    const std::string path1 = network_service::DhcpLeaseStore::path_for(iface, gen1);
    write_lease(path1, gen1, "bound", "1.1.1.1");

    exists = false;
    error.clear();
    ok = expect(network_service::DhcpLeaseStore::read(iface, fact, exists, error),
                "bound lease parse failed") && ok;
    ok = expect(exists, "bound lease should exist") && ok;
    ok = expect(fact.configured(), "bound lease should be configured") && ok;
    ok = expect(fact.generation == gen1, "lease generation mismatch") && ok;
    ok = expect(fact.gateway4 == "192.168.50.1",
                "only first router should enter the v1 fact") && ok;
    ok = expect(fact.dns4 == "1.1.1.1",
                "only first DNS should enter the v1 fact") && ok;

    const std::string first_fingerprint = fact.fingerprint();
    write_lease(path1, gen1, "renew", "9.9.9.9");
    ok = expect(network_service::DhcpLeaseStore::read(iface, fact, exists, error),
                "renew lease parse failed") && ok;
    ok = expect(fact.fingerprint() != first_fingerprint,
                "renew with changed DNS must change fingerprint") && ok;

    ok = expect(network_service::DhcpLeaseStore::activate_generation(iface, gen2, error),
                "failed to activate replacement generation") && ok;
    exists = true;
    error.clear();
    ok = expect(network_service::DhcpLeaseStore::read(iface, fact, exists, error),
                "new generation without lease should be a valid empty read") && ok;
    ok = expect(!exists,
                "old generation lease must not satisfy a new DHCP generation") && ok;

    // Simulate a late callback from the stopped DHCP process. It may update its
    // generation-scoped file, but it must never become the current fact.
    write_lease(path1, gen1, "deconfig", "");
    exists = true;
    ok = expect(network_service::DhcpLeaseStore::read(iface, fact, exists, error),
                "stale generation read should not fail") && ok;
    ok = expect(!exists,
                "late callback from stale generation must be ignored") && ok;

    const std::string path2 = network_service::DhcpLeaseStore::path_for(iface, gen2);
    write_lease(path2, gen2, "bound", "8.8.4.4");
    ok = expect(network_service::DhcpLeaseStore::read(iface, fact, exists, error),
                "current generation lease parse failed") && ok;
    ok = expect(exists && fact.generation == gen2 && fact.configured(),
                "current generation lease must win over stale callback") && ok;

    write_lease(path2, gen2, "deconfig", "");
    ok = expect(network_service::DhcpLeaseStore::read(iface, fact, exists, error),
                "deconfig lease parse failed") && ok;
    ok = expect(exists && !fact.configured(),
                "current generation deconfig must remain observable") && ok;

    network_service::DhcpLeaseStore::clear(iface);
    (void)unlink(path1.c_str());
    return ok ? 0 : 1;
}
