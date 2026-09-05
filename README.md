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

The **current production source contract is v0**, using one newline-delimited JSON request and one JSON-line response per accepted connection. It is frozen at:

- `docs/contracts/network-ipc-source-v0.json`
- `docs/contracts/NETWORK_IPC_SOURCE_CONTRACT_V0.md`

Examples:

```json
{"method":"network.ping"}
```

```json
{"method":"network.snapshot"}
```

The platform Network IPC v1 contract is intentionally **not** claimed as implemented yet. The governed migration is documented in:

`docs/migrations/NETWORK_IPC_V1_MIGRATION_PLAN.md`.

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
