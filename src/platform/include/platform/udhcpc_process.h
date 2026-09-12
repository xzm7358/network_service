#ifndef NETWORK_SERVICE_UDHCPC_PROCESS_H
#define NETWORK_SERVICE_UDHCPC_PROCESS_H

#include <string>

namespace network_service {

enum class UdhcpcOwnershipState {
    Absent = 0,
    OwnedRunning,
    StaleArtifacts,
    ConflictingProcess,
};

struct UdhcpcProbeResult {
    UdhcpcOwnershipState state = UdhcpcOwnershipState::Absent;
    int pid = -1;
    std::string generation;
    bool lease_exists = false;
};

class UdhcpcProcess {
public:
    static bool start(const std::string &iface, std::string &error);
    static void stop(const std::string &iface);
    static bool is_running(const std::string &iface);

    // Inspects only NetworkService-owned artifact locations. A process is
    // adoptable only when PID, interface, pidfile, active generation and event
    // script identity all agree. A live DHCP process with incomplete ownership
    // identity is reported as a conflict and is never killed or adopted.
    static bool probe(const std::string &iface,
                      UdhcpcProbeResult &result,
                      std::string &error);

    // Removes dead/stale NetworkService artifacts only. This function never
    // signals a process and is therefore safe during brownfield startup cleanup.
    static void cleanup_stale(const std::string &iface);

    static std::string pidfile_for(const std::string &iface);
    static std::string event_script_path(const std::string &iface,
                                         const std::string &generation);
};

} // namespace network_service

#endif // NETWORK_SERVICE_UDHCPC_PROCESS_H
