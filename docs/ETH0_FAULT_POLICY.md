# eth0 Fault Handling Policy

`eth0` is treated as the protected management link.

## Phase 1: observe/adopt only

When NetworkService starts, it may start after Linux init scripts, `desktop`, MQTT, or telnet have already used `eth0`. Therefore it must adopt the live state and must not perform disruptive recovery.

Allowed:

- Read `eth0` carrier state.
- Read IPv4 address and netmask.
- Read default route and metric.
- Read DNS state.
- Report degraded state in `network.snapshot`.

Forbidden:

- Restart `udhcpc` when `eth0` already has an IPv4 address.
- Flush `eth0` address automatically.
- Delete `eth0` connected routes automatically.
- Rewrite DNS automatically.
- Repair default route automatically.

## Later phases

Explicit operations may be added later:

- `eth.set_static`
- `eth.set_dhcp`
- `route.apply`
- `dns.apply`

Those operations must be user-initiated or policy-initiated with clear ownership and idempotent route/DNS implementation.

## Reason

A background DHCP restart can run a `deconfig` script, temporarily set `eth0` to `0.0.0.0`, remove default routes, and break telnet, MQTT, HTTP, or OTA. That is not acceptable for a wall panel management link.
