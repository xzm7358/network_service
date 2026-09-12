# Network Truth Model Phase 4

## Goal

Separate observed network facts from derived product readiness while keeping the frozen IPC JSON shape compatible.

## Ownership

```text
wpa_supplicant events       kernel/filesystem facts
        |                           |
        v                           v
   WifiL2State              IP / route / DNS facts
        \                           /
         \                         /
          v                       v
             NetworkState normalizer
                      |
                      v
              NetworkSnapshot
              (single truth model)
                      |
              +-------+-------+
              |               |
              v               v
         IPC snapshot     state-change detector
```

### Platform

Platform adapters only observe mechanism facts:

- `WpaEventMonitor` owns `WifiL2State` and scan event counters;
- `interface_snapshot` owns interface existence, carrier, raw IPv4, route and resolver facts;
- platform code must not derive `connected`, `network_ready` or `online` from those facts.

Scanning is not an L2 state. A connected station can scan in the background without losing association truth.

### Service / Policy

`network_state.cpp` is the normalization boundary:

- `WifiL2State::Connected` is distinct from IPv4 readiness;
- `IpState::Configuring` represents L2 success while DHCP is in progress;
- `IpState::Ready` requires a usable IPv4 fact on a usable link;
- an explicit L2 disconnect wins over stale kernel IP/route facts;
- `network_ready` requires a usable selected interface, default route and DNS configuration;
- `network_ready` is not Internet reachability.

`online` remains only as the legacy IPC compatibility projection of `network_ready` until a coordinated contract migration.

## Brownfield startup compatibility

When NetworkService adopts an already-live Wi-Fi interface before observing a supplicant transition, `WifiL2State::Unknown + has_ip` is temporarily treated as a usable legacy link. Once the monitor observes an explicit L2 state, that explicit state becomes authoritative.

A later improvement may initialize L2 truth from a bounded supplicant `STATUS` query, but Phase 4 does not require an additional runtime thread or polling loop.

## Locking rule

`WifiManager` has separate state and operation locks:

- the state mutex protects only `WifiManagerState`;
- no external DHCP callback executes while holding the state mutex;
- the operation mutex serializes DHCP start/stop calls;
- a cancelling transition owns the stop operation.

This prevents lock-order inversion between `WifiManager` and `NetworkControlPlane` while preserving single-owner DHCP lifecycle semantics.

## Compatibility

The JSON shape of `network.snapshot` and `wpa.events` is unchanged in this phase. Legacy fields (`connected`, `online`, `wifi_state`) are compatibility projections from the normalized truth rather than independent sources of truth.

## Regression requirements

- L2 connected + no IP => connected L2, `IpState::Configuring`, not network ready;
- L2 connected + IP + route + DNS => `IpState::Ready` and network ready;
- explicit disconnect + stale IP/route => disconnected and not network ready;
- brownfield Unknown + already-live IP/route remains compatible;
- background scan does not overwrite L2 connection truth;
- concurrent CONNECTED events start at most one DHCP lifecycle;
- DISCONNECTED racing an in-flight DHCP start results in exactly one cancelling stop;
- typed L2/IP transitions participate in `network.state.changed` detection.
