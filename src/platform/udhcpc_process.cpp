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
#include <vector>

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

bool path_exists(const std::string &path) {
    return access(path.c_str(), F_OK) == 0;
}

bool read_pidfile(const std::string &iface, int &pid) {
    pid = -1;
    std::ifstream f(UdhcpcProcess::pidfile_for(iface));
    return static_cast<bool>(f >> pid) && pid > 1;
}

bool read_process_args(int pid, std::vector<std::string> &args) {
    args.clear();
#ifdef __linux__
    if (pid <= 1) return false;
    std::ifstream f("/proc/" + std::to_string(pid) + "/cmdline", std::ios::binary);
    if (!f) return false;
    const std::string cmdline((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
    if (cmdline.empty()) return false;

    std::size_t start = 0;
    while (start < cmdline.size()) {
        const std::size_t end = cmdline.find('\0', start);
        const std::size_t count =
            end == std::string::npos ? cmdline.size() - start : end - start;
        if (count > 0) args.push_back(cmdline.substr(start, count));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return !args.empty();
#else
    (void)pid;
    return false;
#endif
}

bool basename_is_udhcpc(const std::string &arg) {
    const std::size_t slash = arg.find_last_of('/');
    const std::string base = slash == std::string::npos ? arg : arg.substr(slash + 1);
    return base == "udhcpc";
}

bool has_arg(const std::vector<std::string> &args, const std::string &wanted) {
    for (const auto &arg : args) {
        if (arg == wanted) return true;
    }
    return false;
}

bool has_option_value(const std::vector<std::string> &args,
                      const std::string &option,
                      const std::string &value) {
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (args[i - 1] == option && args[i] == value) return true;
    }
    return false;
}

bool matches_udhcpc_iface(const std::vector<std::string> &args,
                          const std::string &iface) {
    bool has_udhcpc = false;
    for (const auto &arg : args) {
        if (basename_is_udhcpc(arg)) {
            has_udhcpc = true;
            break;
        }
    }
    return has_udhcpc && has_option_value(args, "-i", iface);
}

bool matches_owned_process(const std::vector<std::string> &args,
                           const std::string &iface,
                           const std::string &generation) {
    if (!matches_udhcpc_iface(args, iface) || generation.empty()) return false;
    return has_arg(args, "-f") &&
           has_option_value(args, "-p", UdhcpcProcess::pidfile_for(iface)) &&
           has_option_value(args,
                            "-s",
                            UdhcpcProcess::event_script_path(iface, generation));
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

bool UdhcpcProcess::probe(const std::string &iface,
                          UdhcpcProbeResult &result,
                          std::string &error) {
    result = UdhcpcProbeResult{};
    error.clear();
    if (!is_safe_iface(iface)) {
        error = "invalid DHCP iface";
        return false;
    }

    std::string generation;
    bool generation_exists = false;
    if (!DhcpLeaseStore::active_generation(iface,
                                            generation,
                                            generation_exists,
                                            error)) {
        return false;
    }
    if (generation_exists) {
        result.generation = generation;
        result.lease_exists = path_exists(DhcpLeaseStore::path_for(iface, generation));
    }

    int pid = -1;
    const bool pidfile_exists = path_exists(pidfile_for(iface));
    if (!read_pidfile(iface, pid)) {
        result.state = (pidfile_exists || generation_exists)
                           ? UdhcpcOwnershipState::StaleArtifacts
                           : UdhcpcOwnershipState::Absent;
        return true;
    }
    result.pid = pid;

    std::vector<std::string> args;
    if (!read_process_args(pid, args)) {
        result.state = UdhcpcOwnershipState::StaleArtifacts;
        return true;
    }
    if (!matches_udhcpc_iface(args, iface)) {
        // PID reuse or an unrelated process cannot own DHCP for this interface.
        result.state = UdhcpcOwnershipState::StaleArtifacts;
        return true;
    }

    if (!generation_exists || !matches_owned_process(args, iface, generation) ||
        !path_exists(event_script_path(iface, generation))) {
        // A live DHCP process exists but its identity is not complete enough to
        // adopt or signal safely. Preserve it and surface an ownership conflict.
        result.state = UdhcpcOwnershipState::ConflictingProcess;
        return true;
    }

    result.state = UdhcpcOwnershipState::OwnedRunning;
    return true;
}

bool UdhcpcProcess::is_running(const std::string &iface) {
    UdhcpcProbeResult result;
    std::string ignored;
    return probe(iface, result, ignored) &&
           result.state == UdhcpcOwnershipState::OwnedRunning;
}

void UdhcpcProcess::cleanup_stale(const std::string &iface) {
    if (!is_safe_iface(iface)) return;

    std::string generation;
    bool generation_exists = false;
    std::string ignored;
    (void)DhcpLeaseStore::active_generation(iface,
                                            generation,
                                            generation_exists,
                                            ignored);

    (void)unlink(pidfile_for(iface).c_str());
    if (generation_exists) {
        (void)unlink(event_script_path(iface, generation).c_str());
    }
    DhcpLeaseStore::clear(iface);
}

void UdhcpcProcess::stop(const std::string &iface) {
    if (!is_safe_iface(iface)) return;

    UdhcpcProbeResult probe_result;
    std::string ignored;
    if (!probe(iface, probe_result, ignored)) return;

    if (probe_result.state == UdhcpcOwnershipState::ConflictingProcess) {
        // Ownership identity is incomplete. Never kill or erase another owner's
        // live DHCP process merely because our pidfile path references it.
        return;
    }

    if (probe_result.state == UdhcpcOwnershipState::OwnedRunning) {
        (void)kill(probe_result.pid, SIGTERM);
        for (int i = 0; i < 20; ++i) {
            std::vector<std::string> args;
            if (!read_process_args(probe_result.pid, args) ||
                !matches_owned_process(args, iface, probe_result.generation)) {
                break;
            }
            usleep(25 * 1000);
        }

        std::vector<std::string> args;
        if (read_process_args(probe_result.pid, args) &&
            matches_owned_process(args, iface, probe_result.generation)) {
            (void)kill(probe_result.pid, SIGKILL);
        }
    }

    cleanup_stale(iface);
}

bool UdhcpcProcess::start(const std::string &iface, std::string &error) {
    error.clear();
    if (!is_safe_iface(iface)) {
        error = "invalid DHCP iface";
        return false;
    }

    UdhcpcProbeResult existing;
    if (!probe(iface, existing, error)) return false;
    if (existing.state == UdhcpcOwnershipState::ConflictingProcess) {
        error = "conflicting DHCP process prevents NetworkService ownership";
        return false;
    }
    if (existing.state == UdhcpcOwnershipState::OwnedRunning) {
        stop(iface);
    } else if (existing.state == UdhcpcOwnershipState::StaleArtifacts) {
        cleanup_stale(iface);
    }

    const std::string generation = next_generation();
    if (!DhcpLeaseStore::activate_generation(iface, generation, error)) return false;
    if (!ensure_event_script(iface, generation, error)) {
        DhcpLeaseStore::clear(iface);
        return false;
    }

    const std::string script_path = event_script_path(iface, generation);
    std::ostringstream os;
    // Keep the DHCP client in the foreground relative to its own process so the
    // pidfile describes the long-lived client rather than a daemonization parent.
    // The shell backgrounding is only used to keep NetworkService non-blocking.
    os << "udhcpc -f -i " << iface
       << " -t 15 -n -p " << shell_quote(pidfile_for(iface))
       << " -s " << shell_quote(script_path) << " &";
    if (!run_command(os.str(), error)) {
        (void)unlink(script_path.c_str());
        DhcpLeaseStore::clear(iface);
        return false;
    }

    // `system("... &")` only proves that the shell accepted the launch. Require
    // observable process truth before reporting a successful mechanism start.
    for (int i = 0; i < 20; ++i) {
        if (is_running(iface)) {
            error.clear();
            return true;
        }
        usleep(25 * 1000);
    }

    error = "udhcpc did not remain running";
    stop(iface);
    return false;
}

} // namespace network_service
