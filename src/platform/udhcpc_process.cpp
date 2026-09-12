#include "platform/udhcpc_process.h"

#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <mutex>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "platform/dhcp_lease_store.h"

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

bool pid_matches_udhcpc_iface(int pid, const std::string &iface) {
#ifdef __linux__
    if (pid <= 1 || !is_safe_iface(iface)) return false;

    std::ifstream f("/proc/" + std::to_string(pid) + "/cmdline", std::ios::binary);
    if (!f) return false;
    const std::string cmdline((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
    if (cmdline.empty()) return false;

    bool has_udhcpc = false;
    bool has_iface = false;
    std::string previous;
    std::size_t start = 0;
    while (start < cmdline.size()) {
        const std::size_t end = cmdline.find('\0', start);
        const std::size_t count =
            end == std::string::npos ? cmdline.size() - start : end - start;
        const std::string arg = cmdline.substr(start, count);
        if (!has_udhcpc && arg.find("udhcpc") != std::string::npos) {
            has_udhcpc = true;
        }
        if (previous == "-i" && arg == iface) {
            has_iface = true;
        }
        previous = arg;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return has_udhcpc && has_iface;
#else
    (void)pid;
    (void)iface;
    return false;
#endif
}

bool ensure_event_script(std::string &error) {
    static std::mutex lock;
    std::lock_guard<std::mutex> guard(lock);

    const std::string path = UdhcpcProcess::event_script_path();
    const std::string tmp = path + "." + std::to_string(getpid()) + ".tmp";
    std::ofstream f(tmp, std::ios::out | std::ios::trunc);
    if (!f) {
        error = "failed to write udhcpc lease-event script";
        return false;
    }

    f << "#!/bin/sh\n"
      << "lease=\"/tmp/network_service_dhcp_${interface}.lease\"\n"
      << "tmp=\"${lease}.$$\"\n"
      << "write_fact() {\n"
      << "  {\n"
      << "    printf 'event=%s\\n' \"$1\"\n"
      << "    printf 'ip=%s\\n' \"${ip:-}\"\n"
      << "    printf 'subnet=%s\\n' \"${subnet:-}\"\n"
      << "    printf 'router=%s\\n' \"${router:-}\"\n"
      << "    printf 'dns=%s\\n' \"${dns:-}\"\n"
      << "  } > \"$tmp\" || exit 1\n"
      << "  mv -f \"$tmp\" \"$lease\" || exit 1\n"
      << "}\n"
      << "case \"$1\" in\n"
      << "  deconfig) write_fact deconfig ;;\n"
      << "  bound|renew) write_fact \"$1\" ;;\n"
      << "esac\n"
      << "exit 0\n";
    f.close();
    if (!f) {
        (void)unlink(tmp.c_str());
        error = "failed to flush udhcpc lease-event script";
        return false;
    }
    if (chmod(tmp.c_str(), 0755) != 0 || rename(tmp.c_str(), path.c_str()) != 0) {
        (void)unlink(tmp.c_str());
        error = "failed to publish udhcpc lease-event script";
        return false;
    }
    return true;
}

} // namespace

std::string UdhcpcProcess::pidfile_for(const std::string &iface) {
    return "/tmp/smart_hmi_udhcpc_" + iface + ".pid";
}

std::string UdhcpcProcess::event_script_path() {
    return "/tmp/network_service_udhcpc_event.script";
}

void UdhcpcProcess::stop(const std::string &iface) {
    if (!is_safe_iface(iface)) return;

    const std::string pidfile = pidfile_for(iface);
    std::ifstream f(pidfile);
    int pid = -1;
    if (f >> pid) {
        if (pid_matches_udhcpc_iface(pid, iface)) {
            (void)kill(pid, SIGTERM);
        }
    }
    (void)unlink(pidfile.c_str());
    DhcpLeaseStore::clear(iface);
}

bool UdhcpcProcess::start(const std::string &iface, std::string &error) {
    error.clear();
    if (!is_safe_iface(iface)) {
        error = "invalid DHCP iface";
        return false;
    }
    if (!ensure_event_script(error)) return false;

    stop(iface);

    std::ostringstream os;
    os << "udhcpc -i " << iface
       << " -t 15 -n -p " << shell_quote(pidfile_for(iface))
       << " -s " << shell_quote(event_script_path()) << " &";
    return run_command(os.str(), error);
}

} // namespace network_service
