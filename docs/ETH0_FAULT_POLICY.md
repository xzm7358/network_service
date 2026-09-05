# eth0 Fault Handling Policy

`eth0` is the protected management link.

## Startup / automatic recovery

When NetworkService starts, it may start after Linux init scripts, `desktop`, MQTT, telnet, HTTP or OTA have already used `eth0`. Startup therefore adopts live state and must not perform disruptive repair merely because the daemon has started.

Without an explicit apply request or a separately reviewed policy decision, NetworkService must not:

- restart `udhcpc` for an already configured management link;
- flush the existing `eth0` address;
- delete connected/default routes;
- rewrite DNS;
- repair route/DNS state automatically.

The daemon may always observe and report carrier, address, route and DNS state.

## Explicit apply operations

The current implementation already supports explicit mutation paths including:

- `eth.set_static`;
- `eth.set_dhcp`;
- Wi-Fi enable/connect/disconnect and DHCP operations.

These operations may invoke platform mechanisms such as `udhcpc`, `ifconfig`, `route`, `wpa_cli` and `/etc/resolv.conf` **inside the NetworkService platform/backend layer**.

This does not authorize other product processes to mutate networking directly. NetworkService remains the intended network-control owner.

## Policy requirement

Any future automatic repair policy must define:

1. state authority;
2. idempotency;
3. readiness criteria;
4. retry/timeout behavior;
5. route/DNS conflict handling;
6. recovery from daemon restart or event gaps;
7. target regression evidence proving that the management link is not disrupted.

## Reason

A background DHCP restart can execute a `deconfig` path, temporarily clear the interface address, replace routes or DNS, and break telnet, MQTT, HTTP or OTA. Explicit user/policy intent and regression evidence are therefore required before disruptive repair is allowed.
