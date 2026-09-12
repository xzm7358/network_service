# NetworkService Runtime Architecture V1

## Status

**Landed baseline.** Runtime migration Phases 1-6 are complete on `main`.

The design target is a small, explicit network control plane suitable for a 64 MB embedded Linux device. The implementation deliberately reuses `wpa_supplicant` and BusyBox `udhcpc` while keeping product lifecycle, route/DNS policy, truth normalization and IPC ownership inside NetworkService.

## Frozen dependency direction

```text
                     IPC
                      |
                      v
             NetworkControlPlane
                 /          \
                v            v
         WifiManager     NetworkState
                |
                v
         Platform Ports
        /       |       \
       v        v        v
   wpa_ctrl   udhcpc   netlink
```

The only allowed dependency direction is:

```text
IPC
 |
 v
Service / Policy
 |
 v
Platform Mechanism
```

Reverse dependencies are forbidden.

The same direction is now encoded in CMake:

```text
NetworkService::IPC
        |
        v
NetworkService::Service
        |
        v
NetworkService::Platform
```

The final `network_service` executable contains the composition entry point and links the IPC target; it no longer recompiles one flat runtime source list.

## Ownership rules

1. **IPC** owns Unix-domain transport, framing, sessions, request/response encoding, event sequencing, backpressure and fan-out. IPC must not invoke `wpa_supplicant`, `udhcpc`, `ifconfig`, route mutation or DNS mutation directly.
2. **Service / Policy** owns orchestration, lifecycle decisions, route/DNS policy and normalized network truth. It consumes facts from Platform and decides what operation happens next.
3. **Platform** owns Linux/daemon/persistence mechanisms only. Platform does not decide retry policy, preferred interface, UI state or product route/DNS policy.
4. `CTRL-EVENT-CONNECTED` is an **L2 fact**. It is not DHCP success and is not network readiness.
5. DHCP process ownership is singular. Wi-Fi and Ethernet share `UdhcpcProcess`; Wi-Fi L2-to-DHCP lifecycle decisions belong to `WifiManager`.
6. The udhcpc callback is a **Lease Fact producer only**. It must not configure route or DNS policy.
7. Network truth keeps **L2, IP, default route and DNS facts separate**. Wi-Fi connection truth must never be inferred as `connected == has_ip`.
8. The runtime keeps the existing bounded `poll()` reactor. The current FD count does not justify adding epoll or a larger event framework.
9. Read-only snapshot/status APIs must remain free of network mutation side effects.
10. External/brownfield network ownership must not be silently overwritten; NetworkService only clears DNS that it can prove it owns.

## Current runtime flow

### Wi-Fi and DHCP lifecycle

```text
wpa_supplicant
      |
      | control-socket events
      v
WpaEventMonitor
      |
      | typed L2 fact
      v
  WifiManager
      |
      | start / stop DHCP policy
      v
NetworkControlPlane
      |
      v
 UdhcpcProcess
      |
      | bound / renew / deconfig
      v
DhcpLeaseStore
      |
      | typed Lease Fact
      v
NetworkControlPlane
```

`UdhcpcProcess` generation-fences every DHCP lifecycle. Late callback events from a stopped client cannot overwrite facts belonging to a newer DHCP generation.

### IP / Route / DNS application

```text
DhcpLeaseStore / static config
             |
             v
     NetworkControlPlane
       |       |       |
       v       v       v
     IPv4    Route    DNS policy
       \       |       /
        \      |      /
         v     v     v
       NetworkConfigurator
              |
              v
        Linux mechanisms
```

Default production route policy remains `EthernetPreferred`:

- managed Ethernet default route metric: 10;
- managed Wi-Fi default route metric: 20;
- Wi-Fi DNS is selected only when policy permits it;
- externally managed Ethernet ownership is preserved during brownfield adoption;
- `WifiPreferred` and `WifiOnly` remain tested internal policy capabilities and are not exposed as a new public IPC mutation in this baseline.

### Network truth

Raw Platform observation does not derive product state.

```text
WPA L2 facts ---------+
                      |
Kernel IP facts ------+--> NetworkState normalization --> authoritative truth
                      |
Kernel route facts ---+
                      |
DNS facts ------------+
```

The normalized model distinguishes:

- `WifiL2State`;
- `IpState`;
- default-route truth;
- DNS truth;
- `network_ready`.

Legacy `connected` and `online` fields remain compatibility projections for existing IPC consumers. `online` is **not** claimed to be Internet reachability.

## Event model and reconciliation

Healthy Linux production targets use an event-driven kernel truth path:

```text
Netlink
  |  RTMGRP_LINK
  |  RTMGRP_IPV4_IFADDR
  |  RTMGRP_IPV4_ROUTE
  v
NetworkDaemon Service facade
  v
existing IPC poll() reactor
  v
immediate authoritative snapshot + semantic diff
  v
network.state.changed
```

The reactor uses three complementary paths:

1. **Netlink immediate path** — link, IPv4 address and default-route changes trigger immediate state observation.
2. **250 ms fast path** — consumes DHCP Lease Fact changes and WPA event-sequence dirtiness without unconditionally reading a full snapshot when Netlink is healthy.
3. **30 s authoritative fallback** — catches dropped Netlink notifications and external DNS-only changes that have no Netlink signal.

If Netlink cannot be opened or later becomes unusable, production automatically restores the pre-Netlink **250 ms full reconciliation/state-observation fallback** rather than silently accepting slower recovery. Deterministic test fixtures with injected snapshots also keep the 250 ms observation path and do not attach to the host Netlink stream.

## Module layout

```text
src/
├── main.cpp
├── ipc/
│   ├── network_ipc_server.*
│   └── network_ipc_v1_*.cpp
├── service/
│   ├── network_control_plane.*
│   ├── network_daemon.*
│   ├── network_daemon_events.cpp
│   ├── network_state.*
│   ├── network_state_change_detector.*
│   ├── wifi_manager.*
│   └── wifi_scan_lifecycle.*
├── platform/
│   ├── wpa_ctrl_client.*
│   ├── wpa_event_monitor.*
│   ├── udhcpc_process.*
│   ├── dhcp_lease_store.*
│   ├── network_configurator.*
│   ├── interface_snapshot.*
│   ├── netlink_monitor.*
│   └── wifi_backend.*
└── config/
    └── ethernet_config.*
```

New abstractions are added only when they establish ownership, remove duplication, enforce a boundary or create a meaningful test seam.

## Migration record

### Phase 1 — Supplicant control convergence — complete

- removed `wpa_cli` / `popen` from Wi-Fi control;
- unified command/event control-socket mechanics behind `WpaCtrlClient`;
- preserved external IPC behavior.

### Phase 2 — DHCP ownership — complete

- introduced shared `UdhcpcProcess` lifecycle ownership;
- moved L2-to-DHCP policy into `WifiManager`;
- removed DHCP responsibility from WPA event parsing;
- added duplicate/concurrent start, stale PID and disconnect/reconnect regression coverage.

### Phase 3 — Route/DNS ownership — complete

- reduced udhcpc callback responsibility to atomic Lease Fact publication;
- added typed `DhcpLeaseStore` facts and generation fencing;
- moved route/DNS policy to `NetworkControlPlane`;
- moved Linux mutation mechanics to `NetworkConfigurator`;
- protected external Ethernet/DNS ownership.

### Phase 4 — Truth model — complete

- introduced typed L2/IP truth;
- removed Platform `connected == has_ip` and `online` derivation;
- made Service normalization the single authoritative truth projection;
- separated scan lifecycle from L2 connection truth;
- fixed WifiManager callback/mutex lock-order hazards.

### Phase 5 — Netlink event path — complete

- added nonblocking link/address/default-route Netlink observation;
- integrated the event FD into the existing `poll()` reactor without a new thread;
- replaced unconditional 250 ms full snapshots with event-driven observation when Netlink is healthy;
- retained deterministic and production fallback paths.

### Phase 6 — Build-graph boundary enforcement — complete

- introduced `NetworkService::Platform`, `NetworkService::Service` and `NetworkService::IPC` CMake targets;
- encoded the allowed dependency direction in `target_link_libraries`;
- replaced raw `pthread` linkage with `Threads::Threads`;
- moved C++17/version/include configuration to target-scoped usage requirements;
- removed repeated full-runtime source lists from executable, integration test and E2E fixture builds.

## Supplicant adapter deviation

The repository still does not carry an SDK-provided `wpa_ctrl.h` / `libwpa_client` build dependency. The runtime therefore uses a small `WpaCtrlClient` Platform adapter that speaks the wpa_supplicant control-socket protocol directly.

The adapter boundary is deliberately narrow. If the target SDK later provides the official `wpa_ctrl` client library, only the Platform adapter implementation should change; Service and IPC must remain untouched.

## Validation gates

Architecture changes are expected to keep all of the following green:

- Governance / architecture-boundary verification;
- strict host build with `-Wall -Wextra -Wpedantic -Werror`;
- CTest unit/integration suite;
- IPC v0/v1 coexistence and full v1 contract gates;
- EVENT sequencing and reconnect/rebase regression;
- bounded outbound backpressure and long-lived client lifecycle regression;
- clang static analysis;
- ASan/UBSan;
- target RC evidence flow for SSD20x.
