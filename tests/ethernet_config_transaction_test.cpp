#include "config/ethernet_config.h"

#include <fstream>
#include <iostream>
#include <sstream>
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

std::string read_file(const std::string &path) {
    std::ifstream input(path);
    std::ostringstream content;
    content << input.rdbuf();
    return content.str();
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
    std::string error;
    ok = expect(network_service::ethernet_config_path(dir) ==
                    dir + "/network-service/ethernet.json",
                "Ethernet authority must use network-service/ethernet.json") && ok;
    ok = expect(read_file(network_service::ethernet_config_path(dir)) ==
                    "{\"mode\":\"static\",\"address\":\"192.0.2.10\","
                    "\"prefix\":24,\"gateway\":\"192.0.2.1\","
                    "\"dns\":[\"192.0.2.53\"]}\n",
                "static config was not serialized with the deployed JSON contract") && ok;

    EthernetConfig dhcp;
    dhcp.iface = "eth-test0";
    dhcp.method = "dhcp";
    ok = expect(network_service::save_ethernet_config(dir, dhcp),
                "failed to write DHCP config") && ok;
    ok = expect(read_file(network_service::ethernet_config_path(dir)) ==
                    "{\"mode\":\"dhcp\"}\n",
                "DHCP config does not match deployed ethernet.json") && ok;
    EthernetConfig loaded_dhcp;
    ok = expect(network_service::load_ethernet_config(
                    dir, "eth-test0", loaded_dhcp, error) &&
                    loaded_dhcp.method == "dhcp",
                "deployed DHCP JSON was not readable") && ok;
    ok = expect(network_service::save_ethernet_config(dir, previous),
                "failed to restore baseline config") && ok;

    EthernetConfig desired = previous;
    desired.ip4 = "198.51.100.20";
    desired.gateway4 = "198.51.100.1";
    desired.dns4 = "198.51.100.53";

    EthernetConfigStage stage;
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

    const std::string migration_dir = dir + "/migration";
    (void)mkdir(migration_dir.c_str(), 0755);
    const std::string legacy_path = migration_dir + "/smart_hmi_ethernet.conf";
    {
        std::ofstream legacy(legacy_path);
        legacy << "iface=eth-migrate0\n"
               << "method=static\n"
               << "ip4=203.0.113.20\n"
               << "netmask4=255.255.255.0\n"
               << "gateway4=203.0.113.1\n"
               << "dns4=203.0.113.53\n"
               << "route_metric=10\n"
               << "dns_enabled=1\n";
    }
    EthernetConfig migrated =
        network_service::load_ethernet_config(migration_dir, "eth-migrate0");
    ok = expect(migrated.method == "static" &&
                    migrated.ip4 == "203.0.113.20" &&
                    migrated.netmask4 == "255.255.255.0",
                "legacy config was not imported") && ok;
    ok = expect(access(network_service::ethernet_config_path(migration_dir).c_str(), F_OK) == 0,
                "legacy import did not publish authoritative JSON") && ok;

    {
        std::ofstream legacy(legacy_path, std::ios::trunc);
        legacy << "method=dhcp\n";
    }
    migrated = network_service::load_ethernet_config(migration_dir, "eth-migrate0");
    ok = expect(migrated.method == "static" && migrated.ip4 == "203.0.113.20",
                "legacy config overrode existing authoritative JSON") && ok;

    const std::string invalid_dir = dir + "/invalid";
    const std::string invalid_authority_dir = invalid_dir + "/network-service";
    (void)mkdir(invalid_dir.c_str(), 0755);
    (void)mkdir(invalid_authority_dir.c_str(), 0755);
    {
        std::ofstream invalid(invalid_authority_dir + "/ethernet.json");
        invalid << "{\"mode\":\"static\",\"address\":\"bad\"}";
        std::ofstream legacy(invalid_dir + "/smart_hmi_ethernet.conf");
        legacy << "method=dhcp\n";
    }
    EthernetConfig invalid;
    error.clear();
    ok = expect(!network_service::load_ethernet_config(
                    invalid_dir, "eth-invalid0", invalid, error) && !error.empty(),
                "invalid authoritative JSON must fail explicitly") && ok;
    ok = expect(access((invalid_dir + "/smart_hmi_ethernet.conf").c_str(), F_OK) == 0,
                "invalid authoritative JSON must not consume legacy config") && ok;

    EthernetConfig invalid_mask = previous;
    invalid_mask.netmask4 = "255.0.255.0";
    EthernetConfigStage invalid_stage;
    error.clear();
    ok = expect(!network_service::stage_ethernet_config(
                    dir, invalid_mask, invalid_stage, error) && !error.empty(),
                "non-contiguous static netmask must be rejected") && ok;

    (void)unlink(network_service::ethernet_config_path(dir).c_str());
    (void)unlink(network_service::ethernet_config_path(migration_dir).c_str());
    (void)unlink(legacy_path.c_str());
    (void)rmdir((migration_dir + "/network-service").c_str());
    (void)rmdir(migration_dir.c_str());
    (void)unlink((invalid_authority_dir + "/ethernet.json").c_str());
    (void)unlink((invalid_dir + "/smart_hmi_ethernet.conf").c_str());
    (void)rmdir(invalid_authority_dir.c_str());
    (void)rmdir(invalid_dir.c_str());
    (void)rmdir((dir + "/network-service").c_str());
    (void)rmdir(dir.c_str());
    return ok ? 0 : 1;
}
