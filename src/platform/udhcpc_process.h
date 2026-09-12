#ifndef NETWORK_SERVICE_UDHCPC_PROCESS_H
#define NETWORK_SERVICE_UDHCPC_PROCESS_H

#include <string>

namespace network_service {

class UdhcpcProcess {
public:
    static bool start(const std::string &iface, std::string &error);
    static void stop(const std::string &iface);

    static std::string pidfile_for(const std::string &iface);
    static std::string event_script_path();
};

} // namespace network_service

#endif // NETWORK_SERVICE_UDHCPC_PROCESS_H
