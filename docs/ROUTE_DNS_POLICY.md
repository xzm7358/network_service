# Route / DNS Policy

## Ownership

Route and DNS selection are owned by `NetworkControlPlane` in the Service/Policy layer.

The dependency direction is:

```text
udhcpc callback
      |
      v
DHCP Lease Fact
      |
      v
NetworkControlPlane
   /          \
  v            v
Route policy  DNS policy
  |            |
  v            v
NetworkConfigurator (platform mechanism)
```

The udhcpc callback is transport/plumbing only. It may publish `bound`, `renew`, and `deconfig` lease facts containing IP, subnet, router, and DNS values. It must not run `ifconfig`, mutate default routes, or write `/etc/resolv.conf`.

Read-only snapshot and event APIs remain side-effect free. Lease facts are reconciled by the existing bounded reactor cadence before the authoritative snapshot is observed.

## Default production policy

The externally visible default remains **Ethernet Preferred**:

- managed `eth0` default-route metric: `10`;
- managed `wlan0` default-route metric: `20`;
- DNS follows Ethernet while a managed Ethernet default route is active;
- if managed Ethernet deconfigures, DNS may fail over to an active managed Wi-Fi lease;
- if an already-live Ethernet default route exists outside the current NetworkControlPlane ownership, Wi-Fi must not overwrite its DNS during brownfield adoption;
- Wi-Fi must not restart Ethernet DHCP or delete an Ethernet route as a side effect of Wi-Fi DHCP.

## Internal policy support

`NetworkControlPlane` also supports and regression-tests:

- `EthernetPreferred`: Ethernet metric 10, Wi-Fi metric 20, DNS follows Ethernet when available;
- `WifiPreferred`: Wi-Fi metric 10, Ethernet metric 20, DNS follows Wi-Fi when available;
- `WifiOnly`: managed Ethernet default route is removed, Wi-Fi metric 10, DNS follows Wi-Fi;
- `ManualMetric`: retained for the existing type contract; production IPC does not expose a new policy mutation in this phase.

No new route-policy IPC is introduced by Phase 3. The current production behavior therefore remains Ethernet Preferred until a separately reviewed contract adds policy mutation.

## Lease application order

For a new `bound` or `renew` fact, the control plane applies:

```text
Lease Fact
  -> IPv4 address/netmask
  -> default route according to RoutePolicy
  -> DNS according to the selected primary managed link
```

For `deconfig` or an explicit DHCP stop:

```text
stop/lease deconfig
  -> remove that interface's managed default route
  -> clear that interface's IPv4 address
  -> recompute DNS/failover from remaining managed link state
```

Unchanged lease facts are fingerprinted and ignored so the 250 ms reconciliation cadence does not repeatedly mutate network state.
