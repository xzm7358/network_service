#include "service/ethernet_manager.h"

#include <chrono>
#include <iostream>
#include <string>

namespace {

bool expect(bool condition, const char *message) {
    if (condition) return true;
    std::cerr << "ethernet_manager_test: " << message << std::endl;
    return false;
}

} // namespace

int main() {
    using namespace std::chrono;
    using network_service::DhcpClientState;
    using network_service::EthernetManager;

    bool ok = true;

    {
        int stops = 0;
        bool running = true;
        EthernetManager manager(
            [](std::string &) { return true; },
            [&]() {
                ++stops;
                running = false;
            },
            [&]() { return running; });
        manager.adopt_dhcp_running();

        ok = expect(manager.reconcile(false),
                    "carrier down did not change the DHCP lifecycle") && ok;
        const auto state = manager.state();
        ok = expect(stops == 1, "carrier down did not stop DHCP exactly once") && ok;
        ok = expect(!state.carrier_up && state.dhcp_state == DhcpClientState::Idle,
                    "carrier down did not return DHCP to idle") && ok;
        ok = expect(!state.retry_scheduled,
                    "carrier down did not cancel pending retries") && ok;
    }

    {
        steady_clock::time_point now{};
        int starts = 0;
        bool running = true;
        EthernetManager manager(
            [&](std::string &) {
                ++starts;
                running = true;
                return true;
            },
            []() {},
            [&]() { return running; },
            [&]() { return now; },
            milliseconds(100),
            milliseconds(400),
            3);
        manager.adopt_dhcp_running();

        ok = expect(!manager.reconcile(true),
                    "stable running DHCP unexpectedly changed lifecycle") && ok;
        running = false;
        ok = expect(manager.reconcile(true), "DHCP exit was not detected") && ok;
        auto state = manager.state();
        ok = expect(state.dhcp_state == DhcpClientState::Failed &&
                        state.retry_scheduled,
                    "DHCP exit did not schedule a retry") && ok;
        ok = expect(starts == 0, "DHCP exit retried without backoff") && ok;

        now += milliseconds(99);
        ok = expect(!manager.reconcile(true), "retry fired before its deadline") && ok;
        ok = expect(starts == 0, "early reconcile started DHCP") && ok;

        now += milliseconds(1);
        ok = expect(manager.reconcile(true), "due retry did not change lifecycle") && ok;
        state = manager.state();
        ok = expect(starts == 1 && state.dhcp_state == DhcpClientState::Running,
                    "due retry did not restore DHCP") && ok;
    }

    {
        steady_clock::time_point now{};
        int starts = 0;
        EthernetManager manager(
            [&](std::string &error) {
                ++starts;
                error = "spawn failed";
                return false;
            },
            []() {},
            []() { return false; },
            [&]() { return now; },
            milliseconds(100),
            milliseconds(400),
            3);

        std::string error;
        ok = expect(!manager.start_dhcp_now(error),
                    "failed initial start unexpectedly succeeded") && ok;
        ok = expect(starts == 1, "initial DHCP attempt was not counted") && ok;

        now += milliseconds(100);
        (void)manager.reconcile(true);
        now += milliseconds(200);
        (void)manager.reconcile(true);
        now += milliseconds(400);
        (void)manager.reconcile(true);
        now += seconds(10);
        (void)manager.reconcile(true);

        const auto state = manager.state();
        ok = expect(starts == 4,
                    "bounded retry attempted more or fewer than three retries") && ok;
        ok = expect(state.retry_attempts == 3 && !state.retry_scheduled,
                    "retry budget was not exhausted deterministically") && ok;
        ok = expect(state.failure_reason.find("dhcp_start_failed") == 0,
                    "terminal DHCP failure reason was lost") && ok;
    }

    {
        steady_clock::time_point now{};
        int starts = 0;
        int stops = 0;
        bool running = false;
        EthernetManager manager(
            [&](std::string &) {
                ++starts;
                running = true;
                return true;
            },
            [&]() {
                ++stops;
                running = false;
            },
            [&]() { return running; },
            [&]() { return now; },
            milliseconds(100),
            milliseconds(400),
            3);

        std::string error;
        ok = expect(manager.start_dhcp_now(error), "fixture could not start DHCP") && ok;
        ok = expect(manager.reconcile(false), "carrier down was not observed") && ok;
        ok = expect(stops == 1, "carrier loss did not clear owned runtime") && ok;
        ok = expect(manager.reconcile(true), "carrier restoration did not restart DHCP") && ok;
        ok = expect(starts == 2, "carrier restoration did not start a fresh lifecycle") && ok;

        running = false;
        (void)manager.reconcile(true);
        manager.set_static_mode();
        now += seconds(10);
        (void)manager.reconcile(true);
        const auto state = manager.state();
        ok = expect(starts == 2 && !state.dhcp_enabled && !state.retry_scheduled,
                    "static config change did not cancel DHCP retry") && ok;
    }

    {
        int clears = 0;
        int static_applies = 0;
        EthernetManager manager(
            [](std::string &) { return true; },
            [&]() { ++clears; },
            []() { return false; });
        manager.set_static_mode([&](std::string &) {
            ++static_applies;
            return true;
        });

        ok = expect(manager.reconcile(false),
                    "static carrier loss did not change lifecycle") && ok;
        ok = expect(clears == 1,
                    "static carrier loss did not clear owned IP and route") && ok;
        ok = expect(manager.reconcile(true),
                    "static carrier restoration did not change lifecycle") && ok;
        const auto state = manager.state();
        ok = expect(static_applies == 1 && state.static_runtime_active,
                    "static carrier restoration did not replay config") && ok;
    }

    return ok ? 0 : 1;
}
