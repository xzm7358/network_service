#include "platform/udhcpc_process.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
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

std::string next_generation() {
    static std::atomic<unsigned long long> sequence{0};
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    std::ostringstream os;
    os << static_cast<unsigned long>(getpid()) << '_'
       << static_cast<unsigned long long>(ticks) << '_'
       << sequence.fetch_add(1, std::memory_order_relaxed);
    return os.str();
}

bool ensure_event_script(const std::string &iface,
                         const std::string &generation,
                         std::string &error) {
    static std::mutex lock;
    std::lock_guard<std::mutex> guard(lock);

    const std::string path = UdhcpcProcess::event_script_path(iface, generation);
    const std::string active = DhcpLeaseStore::generation_path_for(iface);
    const std::string lease = DhcpLeaseStore::path_for(iface, generation);
    const std::string tmp = path + "." + std::to_string(getpid()) + ".tmp";
    std::ofstream f(tmp, std::ios::out | std::ios::trunc);
    if (!f) {
        error = "failed to write udhcpc lease-event script";
        return false;
    }

    f << "#!/bin/sh\n"
      << "active='" << active << "'\n"
      << "lease='" << lease << "'\n"
      << "generation='" << generation << "'\n"
      << "tmp=\"${lease}.$$\"\n"
      << "is_current() {\n"
      << "  [ -r \"$active\" ] || return 1\n"
      << "  IFS= read -r current < \"$active\" || return 1\n"
      << "  [ \"$current\" = \"$generation\" ]\n"
      << "}\n"
      << "write_fact() {\n"
      << "  is_current || exit 0\n"
      << "  {\n"
      << "    printf 'generation=%s\\n' \"$generation\"\n"
      << "    printf 'event=%s\\n' \"$1\"\n"
      << "    printf 'ip=%s\\n' \"${ip:-}\"\n"
      << "    printf 'subnet=%s\\n' \"${subnet:-}\"\n"
      << "    printf 'router=%s\\n' \"${router:-}\"\n"
      << "    printf 'dns=%s\\n' \"${dns:-}\"\n"
      << "  } > \"$tmp\" || exit 1\n"
      << "  is_current || { rm -f \"$tmp\"; exit 0; }\n"
      << "  mv -f \"$tmp\" \"$lease\" || exit 1\n"
      << "  is_current || rm -f \"$lease\"\n"
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
    if (chmod(tmp.c_str(), 0755) != 0 || std::rename(tmp.c_str(), path.c_str()) != 0) {
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

std::string UdhcpcProcess::event_script_path(const std::string &iface,
                                             const std::string &generation) {
    return "/tmp/network_service_udhcpc_" + iface + "_" + generation + ".script";
}

void UdhcpcProcess::stop(const std::string &iface) {
    if (!is_safe_iface(iface)) return;

    std::string generation;
    bool generation_exists = false;
    std::string ignored;
    (void)DhcpLeaseStore::active_generation(iface,
                                            generation,
                                            generation_exists,
                                            ignored);

    const std::string pidfile = pidfile_for(iface);
    std::ifstream f(pidfile);
    int pid = -1;
    if (f >> pid) {
        if (pid_matches_udhcpc_iface(pid, iface)) {
            (void)kill(pid, SIGTERM);
        }
    }
    (void)unlink(pidfile.c_str());

    if (generation_exists) {
        (void)unlink(event_script_path(iface, generation).c_str());
    }
    DhcpLeaseStore::clear(iface);
}

bool UdhcpcProcess::start(const std::string &iface, std::string &error) {
    error.clear();
    if (!is_safe_iface(iface)) {
        error = "invalid DHCP iface";
        return false;
    }

    stop(iface);

    const std::string generation = next_generation();
    if (!DhcpLeaseStore::activate_generation(iface, generation, error)) return false;
    if (!ensure_event_script(iface, generation, error)) {
        DhcpLeaseStore::clear(iface);
        return false;
    }

    const std::string script_path = event_script_path(iface, generation);
    std::ostringstream os;
    os << "udhcpc -i " << iface
       << " -t 15 -n -p " << shell_quote(pidfile_for(iface))
       << " -s " << shell_quote(script_path) << " &";
    if (!run_command(os.str(), error)) {
        (void)unlink(script_path.c_str());
        DhcpLeaseStore::clear(iface);
        return false;
    }
    return true;
}

} // namespace network_service
