#include "service/wifi_profile_policy.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

void require_events(const std::vector<std::string> &actual,
                    const std::vector<std::string> &expected,
                    const char *message) {
    if (actual == expected) return;
    std::cerr << message << "\nexpected:";
    for (const auto &item : expected) std::cerr << ' ' << item;
    std::cerr << "\nactual:";
    for (const auto &item : actual) std::cerr << ' ' << item;
    std::cerr << '\n';
    throw std::runtime_error(message);
}

struct Harness {
    std::vector<std::string> events;
    int profile_id = 7;
    bool disable_all_ok = true;
    bool enable_ok = true;
    bool select_ok = true;
    bool remove_ok = true;
    bool save_ok = true;

    network_service::WifiProfilePolicy make_policy() {
        network_service::WifiProfilePolicyOps ops;
        ops.ensure_interface_up = [this](std::string &) {
            events.push_back("up");
            return true;
        };
        ops.create_profile = [this](const std::string &ssid,
                                    const std::string &password,
                                    std::string &) {
            events.push_back("create:" + ssid + ":" + password);
            return profile_id;
        };
        ops.find_profile = [this](const std::string &ssid, std::string &) {
            events.push_back("find:" + ssid);
            return profile_id;
        };
        ops.disable_all_profiles = [this](std::string &error) {
            events.push_back("disable_all");
            if (!disable_all_ok) error = "disable_all failed";
            return disable_all_ok;
        };
        ops.set_profile_enabled = [this](int id, bool enabled, std::string &error) {
            events.push_back(std::string(enabled ? "enable:" : "disable:") +
                             std::to_string(id));
            if (!enable_ok) error = "set enabled failed";
            return enable_ok;
        };
        ops.select_profile = [this](int id, std::string &error) {
            events.push_back("select:" + std::to_string(id));
            if (!select_ok) error = "select failed";
            return select_ok;
        };
        ops.remove_profile = [this](int id, std::string &error) {
            events.push_back("remove:" + std::to_string(id));
            if (!remove_ok) error = "remove failed";
            return remove_ok;
        };
        ops.save_profiles = [this](std::string &error) {
            events.push_back("save");
            if (!save_ok) error = "save failed";
            return save_ok;
        };
        return network_service::WifiProfilePolicy(std::move(ops));
    }
};

void test_connect_new_owns_selection_and_persistence_policy() {
    Harness h;
    auto policy = h.make_policy();
    std::string error;
    require(policy.connect_new("Home", "secret", error),
            "connect_new should succeed");
    require(error.empty(), "connect_new should clear error on success");
    require_events(h.events,
                   {"up", "create:Home:secret", "disable_all", "enable:7",
                    "select:7", "save"},
                   "connect_new policy order changed");
}

void test_connect_new_preserves_best_effort_compatibility() {
    Harness h;
    h.disable_all_ok = false;
    h.save_ok = false;
    auto policy = h.make_policy();
    std::string error;
    require(policy.connect_new("Home", "secret", error),
            "best-effort isolate/save must not fail connect_new");
    require(error.empty(), "best-effort errors must not escape successful connect_new");
    require_events(h.events,
                   {"up", "create:Home:secret", "disable_all", "enable:7",
                    "select:7", "save"},
                   "best-effort connect_new sequence changed");
}

void test_connect_saved_does_not_persist() {
    Harness h;
    auto policy = h.make_policy();
    std::string error;
    require(policy.connect_saved("Office", error),
            "connect_saved should succeed");
    require_events(h.events,
                   {"up", "find:Office", "disable_all", "enable:7", "select:7"},
                   "connect_saved policy order changed");
}

void test_selection_failure_stops_sequence() {
    Harness h;
    h.enable_ok = false;
    auto policy = h.make_policy();
    std::string error;
    require(!policy.connect_saved("Office", error),
            "enable failure must fail connect_saved");
    require(error == "set enabled failed", "enable failure error changed");
    require_events(h.events,
                   {"up", "find:Office", "disable_all", "enable:7"},
                   "connect_saved must stop after fatal enable failure");
}

void test_forget_requires_persistence() {
    Harness h;
    h.save_ok = false;
    auto policy = h.make_policy();
    std::string error;
    require(!policy.forget("Old", error),
            "forget must fail when persistence fails");
    require(error == "save failed", "forget persistence error changed");
    require_events(h.events,
                   {"find:Old", "remove:7", "save"},
                   "forget policy order changed");
}

void test_autoconnect_owns_enable_and_persist_policy() {
    Harness h;
    auto policy = h.make_policy();
    std::string error;
    require(policy.set_autoconnect("Guest", false, error),
            "autoconnect disable should succeed");
    require_events(h.events,
                   {"find:Guest", "disable:7", "save"},
                   "autoconnect policy order changed");
}

} // namespace

int main() {
    try {
        test_connect_new_owns_selection_and_persistence_policy();
        test_connect_new_preserves_best_effort_compatibility();
        test_connect_saved_does_not_persist();
        test_selection_failure_stops_sequence();
        test_forget_requires_persistence();
        test_autoconnect_owns_enable_and_persist_policy();
        std::cout << "Wi-Fi profile policy tests passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "Wi-Fi profile policy test failed: " << e.what() << '\n';
        return 1;
    }
}
