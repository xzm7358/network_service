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
5. DHCP ownership is singular. Wi-Fi and Ethernet must eventually share one `UdhcpcProcess`/`DhcpManager` lifecycle owner.
6. Route and DNS policy must not live in the udhcpc callback script. DHCP should report lease facts; Service/Policy decides route/DNS application.
7. Network truth must keep L2, IP, route and DNS facts distinct. Do not define Wi-Fi connection truth as `has_ip`.
8. The runtime should prefer the existing bounded `poll()` reactor over adding a more complex event framework while the active FD count remains small.

## Minimal target module boundary

```text
src/
├── main.cpp
├── service/
│   ├── network_control_plane.*
│   ├── wifi_manager.*
│   └── network_state.*
├── platform/
│   ├── wpa_supplicant.*
│   ├── udhcpc_process.*
│   ├── netlink_monitor.*
│   └── dns_config.*
├── ipc/
│   ├── network_ipc_server.*
│   └── ipc_v1/...
└── config/
    └── network_config.*
```

This is a target shape, not a requirement to create empty abstraction classes. New types are introduced only when they establish ownership, remove duplication or create a useful test seam.

## Migration sequence

### Phase 1 - Supplicant control convergence

- Remove `wpa_cli` process execution from the Wi-Fi command path.
- Put command and event control-socket mechanics behind one thin platform adapter.
- Keep current IPC and Service behavior stable.

### Phase 2 - DHCP ownership

- Introduce one udhcpc lifecycle owner shared by Wi-Fi and Ethernet.
- Remove DHCP start/stop responsibility from WPA event monitoring.
- Add regressions for duplicate start, stale PID, disconnect, reconnect and child-process exit.

### Phase 3 - Route/DNS ownership

- Reduce udhcpc script responsibility to lease delivery/configuration plumbing.
- Move route selection and DNS selection into Service/Policy-owned orchestration.
- Cover EthernetPreferred, WifiPreferred and WifiOnly transitions.

### Phase 4 - Truth model

- Separate Wi-Fi L2 state, IP state, default-route state and DNS state.
- Remove ambiguous `connected == has_ip` semantics.
- Remove reconciliation code that exists only because two models currently claim the same truth.

### Phase 5 - Netlink event path

- Add link/address/route observation through Netlink.
- Replace the 250 ms snapshot polling path where event truth is available.
- Keep a low-frequency reconciliation path as a guard against missed events.

## Current Phase 1 deviation

The repository does not currently carry an SDK-provided `wpa_ctrl.h`/`libwpa_client` build dependency. Phase 1 therefore uses a small `WpaCtrlClient` platform adapter that speaks the same wpa_supplicant control-socket protocol and removes `wpa_cli`/`popen` from the runtime path.

The adapter boundary is intentionally narrow so a later target SDK integration can replace its implementation with the official `wpa_ctrl` API without changing Service/Policy callers.
