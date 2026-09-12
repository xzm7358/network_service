#include "service/wifi_manager.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

bool expect(bool condition, const char *message) {
    if (condition) return true;
    std::cerr << "wifi_manager_test: " << message << std::endl;
    return false;
}

} // namespace

int main() {
    using network_service::DhcpClientState;

    bool ok = true;

    {
        std::atomic<int> starts{0};
        std::atomic<int> stops{0};
        network_service::WifiManager manager(
            [&](std::string &) {
                ++starts;
                return true;
            },
            [&]() { ++stops; });

        manager.on_l2_connected();
        manager.on_l2_connected();
        auto state = manager.state();
        ok = expect(starts.load() == 1, "duplicate CONNECTED restarted DHCP") && ok;
        ok = expect(state.l2_connected, "L2 should be connected") && ok;
        ok = expect(state.dhcp_state == DhcpClientState::Running,
                    "DHCP should be running") && ok;
        ok = expect(state.dhcp_requests == 1, "unexpected DHCP request count") && ok;

        manager.on_l2_disconnected();
        state = manager.state();
        ok = expect(stops.load() == 1, "disconnect should stop DHCP once") && ok;
        ok = expect(!state.l2_connected, "L2 should be disconnected") && ok;
        ok = expect(state.dhcp_state == DhcpClientState::Idle,
                    "DHCP lifecycle should return to idle") && ok;

        manager.on_l2_connected();
        state = manager.state();
        ok = expect(starts.load() == 2, "reconnect should start a new DHCP lifecycle") && ok;
        ok = expect(state.dhcp_requests == 2, "reconnect request count mismatch") && ok;

        manager.stop_dhcp();
        manager.stop_dhcp();
        ok = expect(stops.load() == 2, "explicit stop should be idempotent") && ok;
        ok = expect(manager.state().dhcp_state == DhcpClientState::Idle,
                    "explicit stop must leave DHCP idle") && ok;
    }

    {
        network_service::WifiManager manager(
            [](std::string &error) {
                error = "spawn failed";
                return false;
            },
            []() {});

        manager.on_l2_connected();
        const auto state = manager.state();
        ok = expect(state.dhcp_state == DhcpClientState::Failed,
                    "failed DHCP start must enter failed state") && ok;
        ok = expect(state.dhcp_requests == 1, "failed start must count one attempt") && ok;
        ok = expect(state.failure_reason.find("dhcp_start_failed") == 0,
                    "failed start reason missing") && ok;
    }

    {
        std::atomic<int> starts{0};
        network_service::WifiManager manager(
            [&](std::string &) {
                ++starts;
                return true;
            },
            []() {});

        std::vector<std::thread> threads;
        for (int i = 0; i < 8; ++i) {
            threads.emplace_back([&]() { manager.on_l2_connected(); });
        }
        for (auto &thread : threads) thread.join();

        ok = expect(starts.load() == 1,
                    "concurrent CONNECTED events started multiple DHCP clients") && ok;
        ok = expect(manager.state().dhcp_state == DhcpClientState::Running,
                    "concurrent start did not settle in running state") && ok;
    }

    {
        std::mutex gate_lock;
        std::condition_variable gate_cv;
        bool start_entered = false;
        bool release_start = false;
        std::atomic<int> starts{0};
        std::atomic<int> stops{0};

        network_service::WifiManager manager(
            [&](std::string &) {
                ++starts;
                std::unique_lock<std::mutex> lock(gate_lock);
                start_entered = true;
                gate_cv.notify_all();
                gate_cv.wait(lock, [&]() { return release_start; });
                return true;
            },
            [&]() { ++stops; });

        std::thread connect_thread([&]() { manager.on_l2_connected(); });
        {
            std::unique_lock<std::mutex> lock(gate_lock);
            gate_cv.wait(lock, [&]() { return start_entered; });
        }

        std::thread disconnect_thread([&]() { manager.on_l2_disconnected(); });

        // The disconnect updates state before waiting for the serialized
        // platform operation. Observe that transition deterministically.
        for (int i = 0; i < 100 && manager.state().l2_connected; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ok = expect(!manager.state().l2_connected,
                    "disconnect did not publish L2 state while DHCP start blocked") && ok;

        {
            std::lock_guard<std::mutex> lock(gate_lock);
            release_start = true;
        }
        gate_cv.notify_all();
        connect_thread.join();
        disconnect_thread.join();

        const auto state = manager.state();
        ok = expect(starts.load() == 1,
                    "connect/disconnect race started DHCP more than once") && ok;
        ok = expect(stops.load() == 1,
                    "cancelling transition must own exactly one DHCP stop") && ok;
        ok = expect(!state.l2_connected && state.dhcp_state == DhcpClientState::Idle,
                    "disconnect during start left a live DHCP lifecycle") && ok;
    }

    {
        std::atomic<int> starts{0};
        std::atomic<bool> process_running{true};
        network_service::WifiManager manager(
            [&](std::string &) {
                ++starts;
                process_running.store(true);
                return true;
            },
            [&]() { process_running.store(false); },
            [&]() { return process_running.load(); });

        manager.on_l2_connected();
        ok = expect(manager.state().dhcp_state == DhcpClientState::Running,
                    "process-truth fixture did not start running") && ok;
        ok = expect(!manager.reconcile_dhcp_process(),
                    "live DHCP process changed lifecycle unexpectedly") && ok;

        process_running.store(false);
        ok = expect(manager.reconcile_dhcp_process(),
                    "DHCP process exit was not detected") && ok;
        auto state = manager.state();
        ok = expect(state.l2_connected,
                    "DHCP process exit must not invent an L2 disconnect") && ok;
        ok = expect(state.dhcp_state == DhcpClientState::Failed,
                    "DHCP process exit must enter failed lifecycle") && ok;
        ok = expect(state.failure_reason == "dhcp_process_exited",
                    "DHCP process exit diagnostic mismatch") && ok;
        ok = expect(!manager.reconcile_dhcp_process(),
                    "failed lifecycle should not repeatedly republish process exit") && ok;

        // A later real CONNECTED event may initiate a new lifecycle; there is no
        // hidden timer-based retry policy in the process-truth reconciler.
        manager.on_l2_connected();
        state = manager.state();
        ok = expect(starts.load() == 2 && state.dhcp_state == DhcpClientState::Running,
                    "new CONNECTED event did not start a fresh DHCP lifecycle") && ok;
    }

    {
        std::atomic<int> starts{0};
        std::atomic<int> stops{0};
        std::atomic<bool> process_running{true};
        network_service::WifiManager manager(
            [&](std::string &) {
                ++starts;
                return true;
            },
            [&]() { ++stops; },
            [&]() { return process_running.load(); });

        manager.adopt_dhcp_running();
        auto state = manager.state();
        ok = expect(state.dhcp_state == DhcpClientState::Running,
                    "brownfield DHCP process was not imported as running") && ok;
        ok = expect(!state.l2_connected,
                    "DHCP adoption must not invent an L2 connected fact") && ok;
        ok = expect(state.dhcp_requests == 0,
                    "adoption must not count as a newly requested DHCP lifecycle") && ok;

        manager.on_l2_connected();
        state = manager.state();
        ok = expect(starts.load() == 0,
                    "post-adoption CONNECTED event started duplicate DHCP") && ok;
        ok = expect(state.l2_connected && state.dhcp_state == DhcpClientState::Running,
                    "CONNECTED event did not merge with adopted DHCP lifecycle") && ok;

        manager.on_l2_disconnected();
        ok = expect(stops.load() == 1,
                    "disconnect must stop the adopted DHCP lifecycle once") && ok;
    }

    return ok ? 0 : 1;
}
