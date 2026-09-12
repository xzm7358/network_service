#ifndef NETWORK_SERVICE_INTERFACE_SNAPSHOT_H
#define NETWORK_SERVICE_INTERFACE_SNAPSHOT_H

#include "network_service_types.h"

namespace network_service {

NetworkSnapshot read_live_snapshot(const char *eth_iface, const char *wifi_iface);

} // namespace network_service

#endif // NETWORK_SERVICE_INTERFACE_SNAPSHOT_H
