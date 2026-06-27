# NetworkService

`NetworkService` is the standalone network owner for the embedded Linux control panel.

## Phase 1 Scope

This first migration phase is intentionally observe-only:

- Provides a Unix domain socket server.
- Supports `network.ping`.
- Supports `network.snapshot`.
- Reads live `eth0` / `wlan0` state.
- Does not start or stop `udhcpc`.
- Does not call `ifconfig`, `ip`, `route`, `wpa_cli`, or edit `/etc/resolv.conf`.

This is deliberate. `desktop` currently starts after the system already has a live network. A late-starting daemon must adopt live state first, not reconfigure `eth0`.

## IPC

Default socket:

```text
/tmp/smart_hmi_network.sock
```

Request examples:

```json
{"method":"network.ping"}
```

```json
{"method":"network.snapshot"}
```

Each request is newline-delimited. The response is a single JSON line.

## eth0 Fault Handling Policy

`eth0` is the management link. Until Ethernet control is fully migrated into this daemon, fault handling follows these rules:

1. If `eth0` has an IP address, NetworkService must not restart DHCP.
2. If `eth0` has an IP address, NetworkService must not flush the address.
3. If `eth0` has an IP address, NetworkService must not delete connected routes.
4. Missing default route is observed and reported, not repaired in Phase 1.
5. Missing DNS is observed and reported, not repaired in Phase 1.
6. Static/DHCP switching requires an explicit future IPC command and UI confirmation.
7. Automatic repair may only be enabled later after route/DNS operations are made idempotent and tested.

## Migration Priority

1. Create standalone daemon and IPC protocol.
2. Add `smartcontrol` client and switch UI reads to `network.snapshot`.
3. Move Ethernet configuration persistence to NetworkService.
4. Move explicit Ethernet apply actions to NetworkService.
5. Move Wi-Fi scan/connect/disconnect to NetworkService.
6. Move route/DNS policy ownership to NetworkService.
7. Remove direct network HAL usage from `desktop`.
