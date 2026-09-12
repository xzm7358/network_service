#include "service/wifi_manager.h"

#include <atomic>
#include <iostream>
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
        ok = expect(state.dhcp_requested, "DHCP should be requested") && ok;
        ok = expect(state.dhcp_requests == 1, "unexpected DHCP request count") && ok;

        manager.on_l2_disconnected();
        state = manager.state();
        ok = expect(stops.load() == 1, "disconnect should stop DHCP once") && ok;
        ok = expect(!state.l2_connected, "L2 should be disconnected") && ok;
        ok = expect(!state.dhcp_requested, "DHCP request should be cleared") && ok;

        manager.on_l2_connected();
        state = manager.state();
        ok = expect(starts.load() == 2, "reconnect should start a new DHCP lifecycle") && ok;
        ok = expect(state.dhcp_requests == 2, "reconnect request count mismatch") && ok;

        manager.stop_dhcp();
        manager.stop_dhcp();
        ok = expect(stops.load() == 2, "explicit stop should be idempotent") && ok;
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
        ok = expect(!state.dhcp_requested, "failed DHCP start marked requested") && ok;
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
    }

    return ok ? 0 : 1;
}
