#include "service/wifi_profile_policy.h"

#include <utility>

namespace network_service {

WifiProfilePolicy::WifiProfilePolicy(WifiProfilePolicyOps ops)
    : ops_(std::move(ops)) {}

bool WifiProfilePolicy::activate_profile(int network_id,
                                         bool persist,
                                         std::string &error) const {
    error.clear();
    if (network_id < 0) {
        error = "invalid Wi-Fi profile id";
        return false;
    }
    if (!ops_.disable_all_profiles || !ops_.set_profile_enabled ||
        !ops_.select_profile) {
        error = "Wi-Fi profile platform ports are unavailable";
        return false;
    }

    // Preserve the existing product behavior: isolating the requested profile is
    // best-effort. Failure here must not make an otherwise selectable profile fail.
    std::string ignored;
    (void)ops_.disable_all_profiles(ignored);

    if (!ops_.set_profile_enabled(network_id, true, error)) return false;
    if (!ops_.select_profile(network_id, error)) return false;

    // New credentials were historically persisted best-effort. Keep that
    // compatibility while making the persistence decision a Service policy.
    if (persist && ops_.save_profiles) {
        ignored.clear();
        (void)ops_.save_profiles(ignored);
    }

    error.clear();
    return true;
}

bool WifiProfilePolicy::connect_new(const std::string &ssid,
                                    const std::string &password,
                                    std::string &error) const {
    error.clear();
    if (ssid.empty()) {
        error = "ssid is required";
        return false;
    }
    if (!ops_.ensure_interface_up || !ops_.create_profile) {
        error = "Wi-Fi profile platform ports are unavailable";
        return false;
    }
    if (!ops_.ensure_interface_up(error)) return false;

    const int network_id = ops_.create_profile(ssid, password, error);
    if (network_id < 0) return false;

    if (activate_profile(network_id, true, error)) return true;

    // `connect_new` owns the just-created profile until activation succeeds.
    // Never leave that transient resource behind after ENABLE/SELECT failure.
    // Preserve the original activation diagnostic even if rollback itself fails.
    const std::string failure = error;
    std::string rollback_error;
    if (ops_.remove_profile) {
        (void)ops_.remove_profile(network_id, rollback_error);
        // If removal succeeded, best-effort persistence prevents a failed new
        // connection from reappearing after supplicant restart. This is cleanup,
        // not the success path's persistence guarantee.
        if (rollback_error.empty() && ops_.save_profiles) {
            std::string ignored;
            (void)ops_.save_profiles(ignored);
        }
    }
    error = failure;
    return false;
}

bool WifiProfilePolicy::connect_saved(const std::string &ssid,
                                      std::string &error) const {
    error.clear();
    if (ssid.empty()) {
        error = "ssid is required";
        return false;
    }
    if (!ops_.ensure_interface_up || !ops_.find_profile) {
        error = "Wi-Fi profile platform ports are unavailable";
        return false;
    }
    if (!ops_.ensure_interface_up(error)) return false;

    const int network_id = ops_.find_profile(ssid, error);
    if (network_id < 0) return false;
    return activate_profile(network_id, false, error);
}

bool WifiProfilePolicy::save(const std::string &ssid,
                             const std::string &password,
                             bool autoconnect,
                             std::string &error) const {
    error.clear();
    if (ssid.empty()) {
        error = "ssid is required";
        return false;
    }
    if (!ops_.ensure_interface_up || !ops_.find_profile || !ops_.create_profile ||
        !ops_.configure_profile || !ops_.set_profile_enabled ||
        !ops_.remove_profile || !ops_.save_profiles) {
        error = "Wi-Fi profile platform ports are unavailable";
        return false;
    }
    if (!ops_.ensure_interface_up(error)) return false;

    int network_id = ops_.find_profile(ssid, error);
    bool created = false;
    if (network_id < 0) {
        if (error.find("not found") == std::string::npos) return false;
        error.clear();
        network_id = ops_.create_profile(ssid, password, error);
        if (network_id < 0) return false;
        created = true;
    } else if (!ops_.configure_profile(network_id, ssid, password, error)) {
        return false;
    }

    if (ops_.set_profile_enabled(network_id, autoconnect, error) &&
        ops_.save_profiles(error)) return true;

    if (created) {
        const std::string failure = error;
        std::string ignored;
        (void)ops_.remove_profile(network_id, ignored);
        error = failure;
    }
    return false;
}

bool WifiProfilePolicy::forget(const std::string &ssid, std::string &error) const {
    error.clear();
    if (ssid.empty()) {
        error = "ssid is required";
        return false;
    }
    if (!ops_.find_profile || !ops_.remove_profile || !ops_.save_profiles) {
        error = "Wi-Fi profile platform ports are unavailable";
        return false;
    }

    const int network_id = ops_.find_profile(ssid, error);
    if (network_id < 0) return false;
    if (!ops_.remove_profile(network_id, error)) return false;
    return ops_.save_profiles(error);
}

bool WifiProfilePolicy::set_autoconnect(const std::string &ssid,
                                        bool enabled,
                                        std::string &error) const {
    error.clear();
    if (ssid.empty()) {
        error = "ssid is required";
        return false;
    }
    if (!ops_.find_profile || !ops_.set_profile_enabled || !ops_.save_profiles) {
        error = "Wi-Fi profile platform ports are unavailable";
        return false;
    }

    const int network_id = ops_.find_profile(ssid, error);
    if (network_id < 0) return false;
    if (!ops_.set_profile_enabled(network_id, enabled, error)) return false;
    return ops_.save_profiles(error);
}

} // namespace network_service
