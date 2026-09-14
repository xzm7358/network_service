#include "service/ethernet_startup.h"

#include <iostream>
#include <string>
#include <utility>

namespace {

bool expect(bool condition, const char *message) {
    if (condition) return true;
    std::cerr << "ethernet_startup_test: " << message << std::endl;
    return false;
}

} // namespace

int main() {
    using network_service::EthernetConfig;
    using network_service::EthernetDhcpAdoption;
    using network_service::EthernetStartup;
    using network_service::EthernetStartupOps;

    bool ok = true;

    {
        int starts = 0;
        int static_applies = 0;
        EthernetStartupOps ops;
        ops.load_config = [](EthernetConfig &config, std::string &) {
            config.method = "dhcp";
            return true;
        };
        ops.try_adopt_dhcp = [](std::string &) {
            return EthernetDhcpAdoption::Absent;
        };
        ops.start_dhcp = [&](std::string &) {
            ++starts;
            return true;
        };
        ops.apply_static = [&](const EthernetConfig &, std::string &) {
            ++static_applies;
            return true;
        };

        std::string error;
        EthernetStartup startup(std::move(ops));
        ok = expect(startup.converge(error), "DHCP startup did not converge") && ok;
        ok = expect(starts == 1, "absent DHCP client was not started exactly once") && ok;
        ok = expect(static_applies == 0, "DHCP startup applied static configuration") && ok;
    }

    {
        int starts = 0;
        EthernetStartupOps ops;
        ops.load_config = [](EthernetConfig &config, std::string &) {
            config.method = "dhcp";
            return true;
        };
        ops.try_adopt_dhcp = [](std::string &) {
            return EthernetDhcpAdoption::Adopted;
        };
        ops.start_dhcp = [&](std::string &) {
            ++starts;
            return true;
        };

        std::string error;
        EthernetStartup startup(std::move(ops));
        ok = expect(startup.converge(error), "owned DHCP client was not adopted") && ok;
        ok = expect(starts == 0, "adopted DHCP client was started a second time") && ok;
    }

    {
        int starts = 0;
        EthernetConfig applied;
        EthernetStartupOps ops;
        ops.load_config = [](EthernetConfig &config, std::string &) {
            config.method = "static";
            config.ip4 = "10.20.30.40";
            config.netmask4 = "255.255.255.0";
            config.gateway4 = "10.20.30.1";
            config.dns4 = "1.1.1.1";
            config.route_metric = 10;
            return true;
        };
        ops.try_adopt_dhcp = [](std::string &) {
            return EthernetDhcpAdoption::Absent;
        };
        ops.start_dhcp = [&](std::string &) {
            ++starts;
            return true;
        };
        ops.apply_static = [&](const EthernetConfig &config, std::string &) {
            applied = config;
            return true;
        };

        std::string error;
        EthernetStartup startup(std::move(ops));
        ok = expect(startup.converge(error), "static startup did not converge") && ok;
        ok = expect(starts == 0, "static startup started DHCP") && ok;
        ok = expect(applied.ip4 == "10.20.30.40" && applied.route_metric == 10,
                    "static startup did not apply the authoritative config") && ok;
    }

    {
        int adopts = 0;
        int starts = 0;
        int static_applies = 0;
        EthernetStartupOps ops;
        ops.load_config = [](EthernetConfig &, std::string &error) {
            error = "invalid ethernet.json";
            return false;
        };
        ops.try_adopt_dhcp = [&](std::string &) {
            ++adopts;
            return EthernetDhcpAdoption::Absent;
        };
        ops.start_dhcp = [&](std::string &) {
            ++starts;
            return true;
        };
        ops.apply_static = [&](const EthernetConfig &, std::string &) {
            ++static_applies;
            return true;
        };

        std::string error;
        EthernetStartup startup(std::move(ops));
        ok = expect(!startup.converge(error), "invalid config unexpectedly converged") && ok;
        ok = expect(error == "invalid ethernet.json", "config error was not preserved") && ok;
        ok = expect(adopts == 0 && starts == 0 && static_applies == 0,
                    "invalid config mutated the live brownfield link") && ok;
    }

    return ok ? 0 : 1;
}
