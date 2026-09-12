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
    const std::string pidfile = network_service::UdhcpcProcess::pidfile_for(iface);

    ok = expect(pidfile.find(iface) != std::string::npos,
                "pidfile must remain interface-scoped") && ok;

    {
        std::ofstream out(pidfile, std::ios::out | std::ios::trunc);
        out << static_cast<long>(getpid()) << '\n';
    }

    network_service::UdhcpcProcess::stop(iface);

    errno = 0;
    ok = expect(kill(getpid(), 0) == 0,
                "stale PID ownership check killed an unrelated process") && ok;
    ok = expect(access(pidfile.c_str(), F_OK) != 0,
                "stale pidfile should be removed") && ok;

    std::string error;
    ok = expect(!network_service::UdhcpcProcess::start("bad iface", "/tmp/x", error),
                "invalid interface should be rejected before spawning") && ok;
    ok = expect(!error.empty(), "invalid interface should report an error") && ok;

    error.clear();
    ok = expect(!network_service::UdhcpcProcess::start(iface, "", error),
                "empty script path should be rejected before spawning") && ok;

    return ok ? 0 : 1;
}
