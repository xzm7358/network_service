#include "platform/dhcp_lease_store.h"

#include <fstream>
#include <iostream>
#include <string>

namespace {

bool expect(bool condition, const char *message) {
    if (condition) return true;
    std::cerr << "dhcp_lease_store_test: " << message << std::endl;
    return false;
}

} // namespace

int main() {
    bool ok = true;
    const std::string iface = "utlease0";
    const std::string path = network_service::DhcpLeaseStore::path_for(iface);

    network_service::DhcpLeaseStore::clear(iface);

    network_service::DhcpLeaseFact fact;
    bool exists = true;
    std::string error;
    ok = expect(network_service::DhcpLeaseStore::read(iface, fact, exists, error),
                "missing lease should be a valid empty read") && ok;
    ok = expect(!exists, "missing lease should report exists=false") && ok;

    {
        std::ofstream out(path, std::ios::out | std::ios::trunc);
        out << "event=bound\n"
            << "ip=192.168.50.20\n"
            << "subnet=255.255.255.0\n"
            << "router=192.168.50.1 192.168.50.2\n"
            << "dns=1.1.1.1 8.8.8.8\n";
    }

    exists = false;
    error.clear();
    ok = expect(network_service::DhcpLeaseStore::read(iface, fact, exists, error),
                "bound lease parse failed") && ok;
    ok = expect(exists, "bound lease should exist") && ok;
    ok = expect(fact.configured(), "bound lease should be configured") && ok;
    ok = expect(fact.gateway4 == "192.168.50.1",
                "only first router should enter the v1 fact") && ok;
    ok = expect(fact.dns4 == "1.1.1.1",
                "only first DNS should enter the v1 fact") && ok;

    const std::string first_fingerprint = fact.fingerprint();
    {
        std::ofstream out(path, std::ios::out | std::ios::trunc);
        out << "event=renew\n"
            << "ip=192.168.50.20\n"
            << "subnet=255.255.255.0\n"
            << "router=192.168.50.1\n"
            << "dns=9.9.9.9\n";
    }
    ok = expect(network_service::DhcpLeaseStore::read(iface, fact, exists, error),
                "renew lease parse failed") && ok;
    ok = expect(fact.fingerprint() != first_fingerprint,
                "renew with changed DNS must change fingerprint") && ok;

    {
        std::ofstream out(path, std::ios::out | std::ios::trunc);
        out << "event=deconfig\n"
            << "ip=\nsubnet=\nrouter=\ndns=\n";
    }
    ok = expect(network_service::DhcpLeaseStore::read(iface, fact, exists, error),
                "deconfig lease parse failed") && ok;
    ok = expect(!fact.configured(), "deconfig lease must not be configured") && ok;

    network_service::DhcpLeaseStore::clear(iface);
    return ok ? 0 : 1;
}
