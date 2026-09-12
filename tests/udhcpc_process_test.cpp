#include "platform/dhcp_lease_store.h"
#include "platform/udhcpc_process.h"

#include <cerrno>
#include <csignal>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>

namespace {

bool expect(bool condition, const char *message) {
    if (condition) return true;
    std::cerr << "udhcpc_process_test: " << message << std::endl;
    return false;
}

} // namespace

int main() {
    bool ok = true;
    const std::string iface = "utdhcp0";
    const std::string generation = "test_gen";
    const std::string pidfile = network_service::UdhcpcProcess::pidfile_for(iface);
    const std::string leasefile =
        network_service::DhcpLeaseStore::path_for(iface, generation);
    const std::string generation_file =
        network_service::DhcpLeaseStore::generation_path_for(iface);
    const std::string script =
        network_service::UdhcpcProcess::event_script_path(iface, generation);

    ok = expect(pidfile.find(iface) != std::string::npos,
                "pidfile must remain interface-scoped") && ok;
    ok = expect(script.find(iface) != std::string::npos &&
                script.find(generation) != std::string::npos,
                "event script must be interface/generation scoped") && ok;

    std::string error;
    ok = expect(network_service::DhcpLeaseStore::activate_generation(
                    iface, generation, error),
                "failed to activate test generation") && ok;

    {
        std::ofstream out(pidfile, std::ios::out | std::ios::trunc);
        out << static_cast<long>(getpid()) << '\n';
    }
    {
        std::ofstream out(leasefile, std::ios::out | std::ios::trunc);
        out << "generation=" << generation << "\n"
            << "event=bound\n";
    }
    {
        std::ofstream out(script, std::ios::out | std::ios::trunc);
        out << "#!/bin/sh\nexit 0\n";
    }

    network_service::UdhcpcProcess::stop(iface);

    errno = 0;
    ok = expect(kill(getpid(), 0) == 0,
                "stale PID ownership check killed an unrelated process") && ok;
    ok = expect(access(pidfile.c_str(), F_OK) != 0,
                "stale pidfile should be removed") && ok;
    ok = expect(access(leasefile.c_str(), F_OK) != 0,
                "current lease fact should be removed with DHCP stop") && ok;
    ok = expect(access(generation_file.c_str(), F_OK) != 0,
                "generation marker should be removed with DHCP stop") && ok;
    ok = expect(access(script.c_str(), F_OK) != 0,
                "generation-scoped callback script should be removed with DHCP stop") && ok;

    error.clear();
    ok = expect(!network_service::UdhcpcProcess::start("bad iface", error),
                "invalid interface should be rejected before spawning") && ok;
    ok = expect(!error.empty(), "invalid interface should report an error") && ok;

    return ok ? 0 : 1;
}
