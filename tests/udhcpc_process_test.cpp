#include "platform/dhcp_lease_store.h"
#include "platform/udhcpc_process.h"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {

bool expect(bool condition, const char *message) {
    if (condition) return true;
    std::cerr << "udhcpc_process_test: " << message << std::endl;
    return false;
}

int read_pid(const std::string &path) {
    std::ifstream input(path);
    int pid = -1;
    input >> pid;
    return pid;
}

bool wait_for_pidfile(const std::string &path, int &pid) {
    for (int i = 0; i < 100; ++i) {
        pid = read_pid(path);
        if (pid > 1 && kill(pid, 0) == 0) return true;
        usleep(10 * 1000);
    }
    return false;
}

bool wait_for_exit(int pid) {
    for (int i = 0; i < 100; ++i) {
        errno = 0;
        if (kill(pid, 0) != 0 && errno == ESRCH) return true;
        usleep(10 * 1000);
    }
    return false;
}

bool write_fake_udhcpc(const std::string &path) {
    std::ofstream out(path, std::ios::out | std::ios::trunc);
    if (!out) return false;
    out << "#!/bin/sh\n"
        << "pidfile=''\n"
        << "while [ \"$#\" -gt 0 ]; do\n"
        << "  case \"$1\" in\n"
        << "    -p) shift; pidfile=\"$1\" ;;\n"
        << "  esac\n"
        << "  shift\n"
        << "done\n"
        << "[ -n \"$pidfile\" ] || exit 2\n"
        << "echo $$ > \"$pidfile\"\n"
        << "trap 'exit 0' TERM INT\n"
        << "while :; do sleep 1; done\n";
    out.close();
    return out && chmod(path.c_str(), 0755) == 0;
}

} // namespace

int main() {
    using network_service::UdhcpcOwnershipState;
    using network_service::UdhcpcProbeResult;

    bool ok = true;
    const std::string iface = "utdhcp0";
    const std::string generation = "test_gen";
    const std::string pidfile = network_service::UdhcpcProcess::pidfile_for(iface);
    const std::string leasefile =
        network_service::DhcpLeaseStore::path_for(iface, generation);
    const std::string generation_file =
        network_service::DhcpLeaseStore::generation_path_for(iface);
    const std::string script =
        network_service::UdhcpcProcess::event_script_path(iface, generation);

    ok = expect(pidfile.find(iface) != std::string::npos,
                "pidfile must remain interface-scoped") && ok;
    ok = expect(script.find(iface) != std::string::npos &&
                script.find(generation) != std::string::npos,
                "event script must be interface/generation scoped") && ok;

    std::string error;
    ok = expect(network_service::DhcpLeaseStore::activate_generation(
                    iface, generation, error),
                "failed to activate test generation") && ok;

    {
        std::ofstream out(pidfile, std::ios::out | std::ios::trunc);
        out << static_cast<long>(getpid()) << '\n';
    }
    {
        std::ofstream out(leasefile, std::ios::out | std::ios::trunc);
        out << "generation=" << generation << "\n"
            << "event=bound\n";
    }
    {
        std::ofstream out(script, std::ios::out | std::ios::trunc);
        out << "#!/bin/sh\nexit 0\n";
    }

    UdhcpcProbeResult stale;
    error.clear();
    ok = expect(network_service::UdhcpcProcess::probe(iface, stale, error),
                "stale-artifact probe failed") && ok;
    ok = expect(stale.state == UdhcpcOwnershipState::StaleArtifacts,
                "unrelated PID must be classified as stale artifacts") && ok;
    ok = expect(!network_service::UdhcpcProcess::is_running(iface),
                "unrelated PID must not satisfy udhcpc process truth") && ok;

    network_service::UdhcpcProcess::stop(iface);

    errno = 0;
    ok = expect(kill(getpid(), 0) == 0,
                "stale PID ownership check killed an unrelated process") && ok;
    ok = expect(!network_service::UdhcpcProcess::is_running(iface),
                "stopped DHCP lifecycle must not report a running process") && ok;
    ok = expect(access(pidfile.c_str(), F_OK) != 0,
                "stale pidfile should be removed") && ok;
    ok = expect(access(leasefile.c_str(), F_OK) != 0,
                "current lease fact should be removed with DHCP stop") && ok;
    ok = expect(access(generation_file.c_str(), F_OK) != 0,
                "generation marker should be removed with DHCP stop") && ok;
    ok = expect(access(script.c_str(), F_OK) != 0,
                "generation-scoped callback script should be removed with DHCP stop") && ok;

#if !defined(__linux__)
    std::cout << "udhcpc_process_test: SKIP live ownership requires Linux /proc\n";
    return 77;
#endif

    const std::string fake_dir = "/tmp/network_service_udhcpc_test_" +
                                 std::to_string(static_cast<long>(getpid()));
    const std::string fake_udhcpc = fake_dir + "/udhcpc";
    (void)mkdir(fake_dir.c_str(), 0755);
    ok = expect(write_fake_udhcpc(fake_udhcpc),
                "failed to create fake udhcpc fixture") && ok;

    // A live process is adoptable only when every ownership identity component
    // agrees with NetworkService's active generation.
    error.clear();
    ok = expect(network_service::DhcpLeaseStore::activate_generation(
                    iface, generation, error),
                "failed to reactivate generation for owned-process fixture") && ok;
    {
        std::ofstream out(script, std::ios::out | std::ios::trunc);
        out << "#!/bin/sh\nexit 0\n";
    }
    (void)chmod(script.c_str(), 0755);
    {
        std::ofstream out(leasefile, std::ios::out | std::ios::trunc);
        out << "generation=" << generation << "\n"
            << "event=bound\n"
            << "ip=10.0.0.2\n"
            << "subnet=255.255.255.0\n"
            << "router=10.0.0.1\n"
            << "dns=8.8.8.8\n";
    }

    const std::string owned_command = fake_udhcpc +
        " -f -i " + iface + " -p " + pidfile + " -s " + script + " &";
    ok = expect(std::system(owned_command.c_str()) == 0,
                "failed to launch owned-process fixture") && ok;
    int owned_pid = -1;
    ok = expect(wait_for_pidfile(pidfile, owned_pid),
                "owned-process fixture did not publish pidfile") && ok;

    UdhcpcProbeResult owned;
    error.clear();
    ok = expect(network_service::UdhcpcProcess::probe(iface, owned, error),
                "owned-process probe failed") && ok;
    ok = expect(owned.state == UdhcpcOwnershipState::OwnedRunning,
                "verified NetworkService udhcpc was not adoptable") && ok;
    ok = expect(owned.pid == owned_pid && owned.generation == generation && owned.lease_exists,
                "owned-process probe lost identity/lease facts") && ok;
    ok = expect(network_service::UdhcpcProcess::is_running(iface),
                "verified owned process must satisfy running truth") && ok;

    network_service::UdhcpcProcess::stop(iface);
    ok = expect(wait_for_exit(owned_pid),
                "owned process was not stopped by NetworkService") && ok;
    ok = expect(access(pidfile.c_str(), F_OK) != 0 &&
                access(generation_file.c_str(), F_OK) != 0,
                "owned stop did not clean lifecycle artifacts") && ok;

    // A live udhcpc for the same interface that does not use our active callback
    // identity is a conflict: do not adopt it and, critically, do not kill it.
    error.clear();
    ok = expect(network_service::DhcpLeaseStore::activate_generation(
                    iface, generation, error),
                "failed to activate conflict generation") && ok;
    {
        std::ofstream out(script, std::ios::out | std::ios::trunc);
        out << "#!/bin/sh\nexit 0\n";
    }
    (void)chmod(script.c_str(), 0755);
    const std::string foreign_script = fake_dir + "/foreign.script";
    {
        std::ofstream out(foreign_script, std::ios::out | std::ios::trunc);
        out << "#!/bin/sh\nexit 0\n";
    }
    const std::string conflict_command = fake_udhcpc +
        " -f -i " + iface + " -p " + pidfile + " -s " + foreign_script + " &";
    ok = expect(std::system(conflict_command.c_str()) == 0,
                "failed to launch conflicting-process fixture") && ok;
    int conflict_pid = -1;
    ok = expect(wait_for_pidfile(pidfile, conflict_pid),
                "conflicting-process fixture did not publish pidfile") && ok;

    UdhcpcProbeResult conflict;
    error.clear();
    ok = expect(network_service::UdhcpcProcess::probe(iface, conflict, error),
                "conflicting-process probe failed") && ok;
    ok = expect(conflict.state == UdhcpcOwnershipState::ConflictingProcess,
                "incomplete live ownership identity must be a conflict") && ok;

    network_service::UdhcpcProcess::stop(iface);
    ok = expect(kill(conflict_pid, 0) == 0,
                "NetworkService killed a conflicting external DHCP process") && ok;
    ok = expect(access(pidfile.c_str(), F_OK) == 0,
                "conflict handling must preserve the live process pidfile") && ok;

    (void)kill(conflict_pid, SIGKILL);
    (void)wait_for_exit(conflict_pid);
    network_service::UdhcpcProcess::cleanup_stale(iface);
    (void)unlink(fake_udhcpc.c_str());
    (void)unlink(foreign_script.c_str());
    (void)rmdir(fake_dir.c_str());

    error.clear();
    ok = expect(!network_service::UdhcpcProcess::start("bad iface", error),
                "invalid interface should be rejected before spawning") && ok;
    ok = expect(!error.empty(), "invalid interface should report an error") && ok;

    return ok ? 0 : 1;
}
