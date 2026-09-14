# NetworkService

`NetworkService` is the standalone network owner for the embedded Linux control panel.

## Current Scope

The daemon starts in **adopt-first / explicit-apply** mode:

- starts a Unix domain socket server;
- exposes live `eth0` / `wlan0` snapshots;
- observes WPA events;
- supports explicit Ethernet DHCP/static apply operations;
- supports explicit Wi-Fi enable/scan/connect/connect-saved/disconnect/forget/autoconnect operations;
- owns the backend code that invokes `wpa_cli`, `udhcpc`, `ifconfig`, route mutation and DNS writes for those explicit operations.

Startup itself must not perform disruptive automatic recovery of an already-live management link. Network mutation is performed only through explicit NetworkService operations until a separately reviewed policy layer is introduced.

## IPC

Default socket:

```text
/tmp/smart_hmi_network.sock
```

The frozen production source contract v0 uses one newline-delimited JSON request
and one JSON-line response per accepted connection. It remains available as a
bounded compatibility path at:

- `docs/contracts/network-ipc-source-v0.json`
- `docs/contracts/NETWORK_IPC_SOURCE_CONTRACT_V0.md`

Examples:

```json
{"method":"network.ping"}
```

```json
{"method":"network.snapshot"}
```

New clients use the implemented persistent Network IPC v1 contract: `NSP1`
framing, HELLO/READY negotiation, correlated requests, bounded event delivery,
and snapshot rebase. The contract and migration history are documented in:

- `docs/contracts/NETWORK_IPC_CONTRACT_V1.md`
- `docs/migrations/NETWORK_IPC_V1_MIGRATION_PLAN.md`

The C++ client is exposed as the CMake target `NetworkService::Client` through
`network_service/v1_client.h`.

## Native debug CLI

The `networkctl` target is a native C++ debug client linked directly against
`NetworkService::Client`. It uses the v1 `NSP1` session protocol and has no
JSON or other runtime dependency outside the NetworkService client library.

```text
networkctl [--socket PATH] status
networkctl [--socket PATH] scan
networkctl [--socket PATH] wifi-on
networkctl [--socket PATH] wifi-off
networkctl [--socket PATH] connect <ssid> <psk>
networkctl [--socket PATH] subscribe
```

`status` prints the authoritative snapshot JSON. `scan` starts a scan and
polls its v1 lifecycle until results are ready; service-side failures are
printed unchanged so diagnostics such as `wpa_ctrl connect failed` remain
visible. `subscribe` prints one raw JSON event per line until interrupted.

## EEP Brownfield Adoption

This repository adopts Embedded Engineering Platform release `1.20.0` in brownfield mode. Machine-readable adoption metadata lives in `.eep/`.

The first adoption phase changes governance, documentation and CI only. It does not intentionally change runtime networking behavior.

## eth0 Fault Handling

`eth0` remains the protected management link. See `docs/ETH0_FAULT_POLICY.md`.

The key distinction is:

- **automatic/startup recovery** must not disrupt an already-live link;
- **explicit apply commands** may perform DHCP/static/route/DNS mutation through NetworkService.

## Migration Priorities

1. Freeze the current production IPC source contract.
2. Adopt EEP metadata and product-owned CI.
3. Reconcile documentation with executable behavior.
4. Migrate the newline-delimited IPC to governed Network IPC v1.
5. Add product contract/regression tests.
6. Run NetworkService on the real wall-panel target and collect resource/HIL evidence.
7. Remove remaining direct network mutation paths from other product processes.
