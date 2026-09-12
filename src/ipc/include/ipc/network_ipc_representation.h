#ifndef NETWORK_SERVICE_IPC_REPRESENTATION_H
#define NETWORK_SERVICE_IPC_REPRESENTATION_H

#include <string>
#include <vector>

#include "network_service_contracts.h"
#include "network_service_types.h"
#include "service/network_operation_result.h"
#include "service/network_state_change_detector.h"
#include "service/wifi_scan_lifecycle.h"
#include "service/wpa_events_view.h"

namespace network_service {
namespace ipc_representation {

std::string json_escape(const std::string &value);

std::string snapshot_payload(const NetworkSnapshot &snapshot);
std::string state_changes_payload(const NetworkStateChangeSet &changes);
std::string ping_payload(const PingInfo &info);
std::string wpa_events_payload(const WpaEventsView &view);
std::string ethernet_config_payload(const EthernetConfig &config);
std::string wifi_scan_payload(const std::vector<WifiApRecord> &records);
std::string wifi_saved_payload(const std::vector<WifiSavedNetwork> &records);
std::string wifi_scan_status_payload(const WifiScanStatus &status);
std::string wifi_enabled_payload(const WifiEnabledResult &result);
std::string wifi_command_payload(const WifiCommandResult &result);

std::string v0_success(int status, const std::string &payload_json);
std::string v0_error(int status, const std::string &message);

} // namespace ipc_representation
} // namespace network_service

#endif // NETWORK_SERVICE_IPC_REPRESENTATION_H
