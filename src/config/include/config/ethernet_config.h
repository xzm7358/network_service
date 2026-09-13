#ifndef NETWORK_SERVICE_ETHERNET_CONFIG_H
#define NETWORK_SERVICE_ETHERNET_CONFIG_H

#include <string>

#include "network_service_contracts.h"

namespace network_service {

struct EthernetConfigStage {
    std::string temp_path;
    std::string final_path;
    std::string directory;
    bool valid = false;
};

enum class EthernetConfigCommitResult {
    Failed = 0,
    CommittedDurable,
    CommittedDurabilityUncertain,
};

std::string ethernet_config_path(const std::string &config_dir);
EthernetConfig load_ethernet_config(const std::string &config_dir, const std::string &iface);

// Two-phase durable configuration primitive. Staging writes and fsyncs the full
// new file without changing the committed path. Commit's rename is the logical
// commit point; a later directory-fsync failure means the new file is logically
// committed but sudden-power-loss durability is uncertain.
bool stage_ethernet_config(const std::string &config_dir,
                           const EthernetConfig &config,
                           EthernetConfigStage &stage,
                           std::string &error);
EthernetConfigCommitResult commit_ethernet_config(EthernetConfigStage &stage,
                                                   std::string &error);
void discard_ethernet_config(EthernetConfigStage &stage);

// Legacy one-shot helper retained for non-transactional callers. It reports
// success only when the staged config is committed durably.
bool save_ethernet_config(const std::string &config_dir, const EthernetConfig &config);

} // namespace network_service

#endif // NETWORK_SERVICE_ETHERNET_CONFIG_H
