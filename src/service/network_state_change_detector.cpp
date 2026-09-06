#include "service/network_state_change_detector.h"

#include <sstream>

namespace network_service {
namespace {

bool interface_semantically_equal(const InterfaceSnapshot &lhs,
                                  const InterfaceSnapshot &rhs,
                                  bool include_signal_bars) {
    return lhs.iface == rhs.iface &&
           lhs.exists == rhs.exists &&
           lhs.carrier_up == rhs.carrier_up &&
           lhs.has_ip == rhs.has_ip &&
           lhs.ip4 == rhs.ip4 &&
           lhs.netmask4 == rhs.netmask4 &&
           lhs.has_default_route == rhs.has_default_route &&
           lhs.gateway4 == rhs.gateway4 &&
           lhs.route_metric == rhs.route_metric &&
           lhs.enabled == rhs.enabled &&
           lhs.connected == rhs.connected &&
           lhs.ssid == rhs.ssid &&
           (!include_signal_bars || lhs.signal_bars == rhs.signal_bars);
}

} // namespace

bool NetworkStateChangeSet::any() const {
    return eth || wifi || route || dns;
}

std::string NetworkStateChangeSet::payload_json() const {
    std::ostringstream os;
    os << "{\"changed\":[";
    bool first = true;
    auto append = [&](const char *name, bool enabled) {
        if (!enabled) return;
        if (!first) os << ',';
        os << '"' << name << '"';
        first = false;
    };
    append("eth", eth);
    append("wifi", wifi);
    append("route", route);
    append("dns", dns);
    os << "]}";
    return os.str();
}

NetworkStateChangeSet NetworkStateChangeDetector::observe(
    const NetworkSnapshot &snapshot) {
    NetworkStateChangeSet changes;
    if (!initialized_) {
        previous_ = snapshot;
        initialized_ = true;
        return changes;
    }

    changes.eth = !interface_semantically_equal(previous_.eth, snapshot.eth, false);
    changes.wifi = !interface_semantically_equal(previous_.wifi, snapshot.wifi, true);
    changes.route = previous_.route_policy != snapshot.route_policy ||
                    previous_.primary_iface != snapshot.primary_iface ||
                    previous_.online != snapshot.online;
    changes.dns = previous_.dns_policy != snapshot.dns_policy ||
                  previous_.dns_available != snapshot.dns_available ||
                  previous_.dns4 != snapshot.dns4;

    previous_ = snapshot;
    return changes;
}

void NetworkStateChangeDetector::reset() {
    initialized_ = false;
    previous_ = NetworkSnapshot{};
}

bool NetworkStateChangeDetector::initialized() const {
    return initialized_;
}

} // namespace network_service
