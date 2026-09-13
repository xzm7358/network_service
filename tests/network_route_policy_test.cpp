#include "ipc/network_ipc_v1_business_dispatch.h"
#include "ipc/network_ipc_v1_codec.h"
#include "service/network_daemon.h"

#include <iostream>
#include <string>
#include <vector>

namespace {

bool expect(bool condition, const char *message) {
    if (condition) return true;
    std::cerr << "network_route_policy_test: " << message << std::endl;
    return false;
}

std::string decode_payload(std::vector<std::uint8_t> bytes) {
    network_service::ipc_v1::FrameDecoder decoder;
    const auto status = decoder.feed(bytes);
    if (status == network_service::ipc_v1::DecodeStatus::Error || !decoder.has_frame()) {
        return {};
    }
    return decoder.take_frame().payload;
}

bool contains(const std::string &text, const std::string &needle) {
    return text.find(needle) != std::string::npos;
}

} // namespace

int main() {
    using network_service::NetworkDaemon;
    using network_service::NetworkSnapshot;
    using network_service::RoutePolicy;
    using network_service::ipc_v1::dispatch_business_request;

    bool ok = true;
    NetworkDaemon daemon(
        "eth-test",
        "wifi-test",
        "/tmp/network-route-policy-test",
        "/tmp/network-route-policy-test-no-wpa",
        []() {
            NetworkSnapshot snapshot;
            snapshot.eth.iface = "eth-test";
            snapshot.wifi.iface = "wifi-test";
            return snapshot;
        });

    std::string payload = decode_payload(
        dispatch_business_request(daemon,
                                  1,
                                  "network.route_policy.get",
                                  "{}"));
    ok = expect(contains(payload, "\"status\":200"),
                "initial route policy get did not succeed") && ok;
    ok = expect(contains(payload, "\"policy\":\"ethernet_preferred\""),
                "initial product route policy must be EthernetPreferred") && ok;
    ok = expect(contains(payload, "\"persistent\":false"),
                "route policy contract must state runtime-only persistence") && ok;

    payload = decode_payload(
        dispatch_business_request(daemon,
                                  2,
                                  "network.route_policy.apply",
                                  "{\"policy\":\"wifi_only\"}"));
    ok = expect(contains(payload, "\"status\":200"),
                "wifi_only route policy apply did not succeed") && ok;
    ok = expect(contains(payload, "\"policy\":\"wifi_only\""),
                "route policy apply response did not report wifi_only") && ok;

    payload = decode_payload(
        dispatch_business_request(daemon,
                                  3,
                                  "network.route_policy.get",
                                  "{}"));
    ok = expect(contains(payload, "\"policy\":\"wifi_only\""),
                "route policy get did not observe applied policy") && ok;

    payload = decode_payload(
        dispatch_business_request(daemon,
                                  4,
                                  "network.route_policy.apply",
                                  "{\"policy\":\"manual_metric\"}"));
    ok = expect(contains(payload, "\"status\":400"),
                "manual_metric must not be reachable through product IPC") && ok;
    ok = expect(contains(payload, "\"code\":\"INVALID_PARAMS\""),
                "unsupported policy must return INVALID_PARAMS") && ok;

    payload = decode_payload(
        dispatch_business_request(daemon,
                                  5,
                                  "network.route_policy.apply",
                                  "{\"policy\":\"unknown\"}"));
    ok = expect(contains(payload, "\"status\":400"),
                "unknown route policy must be rejected") && ok;

    const auto service_reject = daemon.route_policy_apply(RoutePolicy::ManualMetric);
    ok = expect(!service_reject.ok() && service_reject.status == 400,
                "Service must reject ManualMetric even if IPC validation is bypassed") && ok;

    return ok ? 0 : 1;
}
