#include "config/ethernet_config.h"

#include <iostream>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace {

bool expect(bool condition, const char *message) {
    if (condition) return true;
    std::cerr << "ethernet_config_transaction_test: " << message << std::endl;
    return false;
}

bool same_config(const network_service::EthernetConfig &lhs,
                 const network_service::EthernetConfig &rhs) {
    return lhs.iface == rhs.iface &&
           lhs.method == rhs.method &&
           lhs.ip4 == rhs.ip4 &&
           lhs.netmask4 == rhs.netmask4 &&
           lhs.gateway4 == rhs.gateway4 &&
           lhs.dns4 == rhs.dns4 &&
           lhs.route_metric == rhs.route_metric &&
           lhs.dns_enabled == rhs.dns_enabled;
}

} // namespace

int main() {
    using network_service::EthernetConfig;
    using network_service::EthernetConfigCommitResult;
    using network_service::EthernetConfigStage;

    bool ok = true;
    const std::string dir =
        "/tmp/network_service_eth_config_txn_" + std::to_string(static_cast<long>(getpid()));
    (void)mkdir(dir.c_str(), 0755);

    EthernetConfig previous;
    previous.iface = "eth-test0";
    previous.method = "static";
    previous.ip4 = "192.0.2.10";
    previous.netmask4 = "255.255.255.0";
    previous.gateway4 = "192.0.2.1";
    previous.dns4 = "192.0.2.53";
    previous.route_metric = 10;
    previous.dns_enabled = true;

    ok = expect(network_service::save_ethernet_config(dir, previous),
                "failed to write baseline config") && ok;

    EthernetConfig desired = previous;
    desired.ip4 = "198.51.100.20";
    desired.gateway4 = "198.51.100.1";
    desired.dns4 = "198.51.100.53";
    desired.route_metric = 20;

    EthernetConfigStage stage;
    std::string error;
    ok = expect(network_service::stage_ethernet_config(dir, desired, stage, error),
                error.c_str()) && ok;
    ok = expect(stage.valid && access(stage.temp_path.c_str(), F_OK) == 0,
                "staged file was not published") && ok;
    ok = expect(same_config(network_service::load_ethernet_config(dir, previous.iface), previous),
                "staging changed committed config before commit") && ok;

    const std::string discarded_path = stage.temp_path;
    network_service::discard_ethernet_config(stage);
    ok = expect(!stage.valid && access(discarded_path.c_str(), F_OK) != 0,
                "discard did not remove staged file") && ok;
    ok = expect(same_config(network_service::load_ethernet_config(dir, previous.iface), previous),
                "discard changed committed config") && ok;

    error.clear();
    ok = expect(network_service::stage_ethernet_config(dir, desired, stage, error),
                error.c_str()) && ok;
    const std::string committed_temp = stage.temp_path;
    const EthernetConfigCommitResult result =
        network_service::commit_ethernet_config(stage, error);
    ok = expect(result != EthernetConfigCommitResult::Failed,
                error.c_str()) && ok;
    ok = expect(!stage.valid && access(committed_temp.c_str(), F_OK) != 0,
                "commit left staged file behind") && ok;
    ok = expect(same_config(network_service::load_ethernet_config(dir, desired.iface), desired),
                "commit did not publish desired config") && ok;

    (void)unlink(network_service::ethernet_config_path(dir).c_str());
    (void)rmdir(dir.c_str());
    return ok ? 0 : 1;
}
