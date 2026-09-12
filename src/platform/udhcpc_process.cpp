#include "platform/udhcpc_process.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>

namespace network_service {
namespace {

bool is_safe_iface(const std::string &value) {
    if (value.empty() || value.size() > 15) return false;
    for (char ch : value) {
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.')) {
            return false;
        }
    }
    return true;
}

std::string shell_quote(const std::string &value) {
    std::string out = "'";
    for (char ch : value) {
        if (ch == '\'') {
            out += "'\\''";
        } else {
            out += ch;
        }
    }
    out += "'";
    return out;
}

bool run_command(const std::string &cmd, std::string &error) {
    const int rc = system(cmd.c_str());
    if (rc != 0) {
        std::ostringstream os;
        os << "command failed rc=" << rc << ": " << cmd;
        error = os.str();
        return false;
    }
    return true;
}

} // namespace

std::string UdhcpcProcess::pidfile_for(const std::string &iface) {
    return "/tmp/smart_hmi_udhcpc_" + iface + ".pid";
}

void UdhcpcProcess::stop(const std::string &iface) {
    if (!is_safe_iface(iface)) return;

    const std::string pidfile = pidfile_for(iface);
    std::ifstream f(pidfile);
    int pid = -1;
    if (f >> pid) {
        if (pid > 1) {
            (void)kill(pid, SIGTERM);
        }
    }
    (void)unlink(pidfile.c_str());
}

bool UdhcpcProcess::start(const std::string &iface,
                          const std::string &script_path,
                          std::string &error) {
    error.clear();
    if (!is_safe_iface(iface)) {
        error = "invalid DHCP iface";
        return false;
    }
    if (script_path.empty()) {
        error = "udhcpc script path is required";
        return false;
    }

    stop(iface);

    std::ostringstream os;
    os << "udhcpc -i " << iface
       << " -t 15 -n -p " << shell_quote(pidfile_for(iface))
       << " -s " << shell_quote(script_path) << " &";
    return run_command(os.str(), error);
}

} // namespace network_service
