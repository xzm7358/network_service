# Route/DNS Ownership Phase 3

## Goal

Move route and DNS policy out of `udhcpc` callbacks while keeping the runtime small enough for the 64 MB target.

## Runtime flow

```text
udhcpc
  |
  v
lease callback script
  |
  | generation-scoped lease fact only
  v
DhcpLeaseStore
  |
  v
NetworkControlPlane
  |-- IPv4 apply/clear
  |-- route policy
  `-- DNS ownership/selection
          |
          v
NetworkConfigurator
```

The callback script MUST NOT call `ifconfig`, mutate routes, or write `/etc/resolv.conf`.

## Generation fencing

Every DHCP start owns a unique generation. The callback script and lease file are generation-scoped. `DhcpLeaseStore` accepts facts only from the currently active generation. This prevents a late `deconfig` callback from a stopped `udhcpc` process from rolling back a new lease.

## DNS ownership

When NetworkService writes `/etc/resolv.conf`, it writes a `# managed-by-network-service` marker. Cleanup only truncates resolver content when that marker is still present. This avoids deleting DNS written by a protected/external Ethernet management path after ownership has changed.

Under `EthernetPreferred`:

- managed Ethernet route + DNS wins;
- otherwise a live external Ethernet default route causes NetworkService to relinquish managed DNS;
- otherwise Wi-Fi route + DNS is used;
- when external Ethernet disappears again, Wi-Fi DNS is actively restored.

## Reactor integration

The existing 250 ms bounded reactor cadence calls `NetworkDaemon::reconcile()` before observing the authoritative snapshot. Snapshot/read APIs themselves remain side-effect free. No new worker thread is introduced.

## Deferred

Phase 4 will separate Wi-Fi L2, IP, route and DNS truth and remove ambiguous `connected == has_ip` semantics. Phase 5 will replace polling with Netlink events where available while retaining low-frequency reconciliation.
