# NetworkService Runtime Architecture V1

## Goal

Keep the runtime small enough for the 64 MB embedded Linux target while preserving strict ownership and dependency direction.

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

## Boundary rules

1. IPC owns transport, framing, sessions, request/response encoding and event fan-out. IPC must not invoke `wpa_supplicant`, `udhcpc`, `ifconfig`, route mutation or DNS mutation directly.
2. Service/Policy owns orchestration and lifecycle decisions. It consumes typed facts from platform mechanisms and decides what operation should happen next.
3. Platform owns Linux and daemon mechanisms only. Platform code must not decide product policy such as retry count, preferred interface, route priority or UI-visible state.
4. WPA control is a Layer-2 mechanism. A `CTRL-EVENT-CONNECTED` event is an L2 fact, not proof of DHCP success or network readiness.
5. DHCP process ownership is singular: Wi-Fi and Ethernet share `UdhcpcProcess`; Wi-Fi L2-to-DHCP lifecycle decisions belong to `WifiManager`.
6. Route and DNS policy must not live in the udhcpc callback script. DHCP reports typed lease facts; `NetworkControlPlane` decides IP/route/DNS application through platform ports.
7. Network truth must keep L2, IP, route and DNS facts distinct. Do not define Wi-Fi connection truth as `has_ip`.
8. The runtime should prefer the existing bounded `poll()` reactor over adding a more complex event framework while the active FD count remains small.

## Current runtime boundary

```text
WpaEventMonitor
      |
      | L2 facts
      v
  WifiManager -----------------------+
      |                               |
      | start/stop DHCP decision      |
      v                               |
NetworkControlPlane <----------------+
      |
      +--> UdhcpcProcess
      |       |
      |       v
      |   Lease Fact files
      |       |
      |<------+   (reconciled by existing 250 ms reactor cadence)
      |
      +--> NetworkConfigurator
              +--> IPv4 mechanism
              +--> default-route mechanism
              +--> DNS mechanism
```

The udhcpc callback performs lease delivery only. It atomically publishes interface-scoped `bound`, `renew`, or `deconfig` facts and does not configure IP, route, or DNS itself.

## Minimal target module boundary

```text
src/
├── main.cpp
├── service/
│   ├── network_control_plane.*
│   ├── wifi_manager.*
│   └── network_state.*        # Phase 4 target
├── platform/
│   ├── wpa_ctrl_client.*
│   ├── udhcpc_process.*
│   ├── dhcp_lease_store.*
│   ├── network_configurator.*
│   └── netlink_monitor.*      # Phase 5 target
├── ipc/
│   ├── network_ipc_server.*
│   └── ipc_v1/...
└── config/
    └── network_config.*
```

This is a target shape, not a requirement to create empty abstraction classes. New types are introduced only when they establish ownership, remove duplication or create a useful test seam.

## Migration status

### Phase 1 - Supplicant control convergence — complete

- `wpa_cli`/`popen` were removed from the Wi-Fi control path.
- Command and event control-socket mechanics are behind `WpaCtrlClient`.
- External IPC behavior stayed stable.

### Phase 2 - DHCP ownership — complete

- `UdhcpcProcess` is the shared Wi-Fi/Ethernet DHCP lifecycle mechanism.
- `WpaEventMonitor` publishes L2 facts only.
- `WifiManager` owns L2-to-DHCP lifecycle decisions and duplicate/concurrent CONNECTED suppression.
- stale PID identity is verified before SIGTERM.

### Phase 3 - Route/DNS ownership — active

- udhcpc callback responsibility is reduced to atomic Lease Fact delivery.
- `DhcpLeaseStore` parses typed lease facts.
- `NetworkControlPlane` owns DHCP lease reconciliation, route priority and DNS selection.
- `NetworkConfigurator` contains the Linux mutation mechanisms only.
- the existing 250 ms reactor cadence performs reconciliation before observing/broadcasting the authoritative snapshot; no new worker thread is added.
- default production policy remains `EthernetPreferred`; `WifiPreferred` and `WifiOnly` are internal/tested policy capabilities and are not exposed through new IPC in this phase.

### Phase 4 - Truth model — next

- Separate Wi-Fi L2 state, IP state, default-route state and DNS state.
- Remove ambiguous `connected == has_ip` semantics.
- Remove reconciliation/overlay code that exists only because two models currently claim the same truth.

### Phase 5 - Netlink event path

- Add link/address/route observation through Netlink.
- Replace the 250 ms snapshot polling path where event truth is available.
- Keep a low-frequency reconciliation path as a guard against missed events.

## Current supplicant deviation

The repository does not currently carry an SDK-provided `wpa_ctrl.h`/`libwpa_client` build dependency. The runtime therefore uses a small `WpaCtrlClient` platform adapter that speaks the same wpa_supplicant control-socket protocol and removes `wpa_cli`/`popen` from the runtime path.

The adapter boundary is intentionally narrow so a later target SDK integration can replace its implementation with the official `wpa_ctrl` API without changing Service/Policy callers.
