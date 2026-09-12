# Thread Model

## Main / IPC reactor thread

The process main thread owns one bounded `poll()` reactor for the Unix-domain listener, the signal wake pipe, all accepted IPC client file descriptors, and the dynamic network-state observation/reconciliation cadence.

The reactor is single-threaded: no per-client worker thread and no EVENT-producer worker thread is created. Each active connection owns protocol-selection state, v1 decoder/session state when applicable, and a bounded outbound queue. Read and write work is budgeted per client per reactor turn so one client cannot monopolize the process.

Protocol/lifecycle rules:

- active client count is bounded by an implementation safety ceiling;
- accepted sockets are non-blocking;
- pre-protocol, incomplete v0, and pre-READY v1 traffic retain a bounded input-idle deadline;
- after successful HELLO/READY, a v1 session has no arbitrary read-idle timeout;
- queued outbound data suppresses further reads for that client until progress is made, preserving bounded backpressure;
- write-stall and queue-overflow terminate only the affected session;
- the signal wake pipe interrupts the reactor, which closes all active clients during shutdown.

### Reactor-owned network reconciliation and state observation

The same reactor owns a `NetworkStateChangeDetector` baseline and a timed deadline. The current host-verifiable cadence is 250 ms.

At each deadline the ordering is:

```text
NetworkDaemon::reconcile()
        |
        | consumes changed DHCP Lease Facts
        v
NetworkControlPlane
        |
        | applies IP / route / DNS policy through platform ports
        v
NetworkDaemon::snapshot()
        |
        v
NetworkStateChangeDetector
        |
        v
network.state.changed EVENT (only when semantics changed)
```

This ordering ensures a DHCP `bound`, `renew`, or `deconfig` fact is applied before the authoritative snapshot is compared and published. `snapshot()` itself remains read-only and has no mutation side effects.

Lease facts are fingerprinted by `NetworkControlPlane`, so an unchanged file observed every 250 ms does not repeatedly apply `ifconfig`, route, or DNS mutations.

This design deliberately adds no DHCP polling thread. Lease application is expected to be infrequent relative to the reactor cadence. Real-target evidence must still verify that the platform mutation commands executed on lease transitions do not create unacceptable IPC/LVGL responsiveness stalls; if they do, execution can later move behind a bounded mechanism executor without changing policy ownership.

The observation/reconciliation deadline participates in the same `poll()` timeout calculation used for connection lifecycle and outbound write-stall deadlines.

EVENT fan-out remains reactor-owned:

- one state transition is encoded exactly once by the generation-global `EventSequencer`;
- the same encoded EVENT/sequence is offered to every currently subscribed healthy v1 session;
- a state transition advances the global sequence watermark even when no client is subscribed, keeping later `network.snapshot.snapshotSeq` truthful;
- globally sequenced subscription control EVENTs are likewise visible to all already-subscribed healthy sessions so one client's subscription cannot create an invisible sequence hole for another;
- a slow/overloaded recipient can be closed by its existing bounded outbound policy without preventing delivery attempts to other healthy recipients.

The 250 ms cadence and host-side client-count/deadline ceilings are implementation safety defaults, not real embedded-target timing or resource evidence.

### Reactor-owned Wi-Fi scan lifecycle

`WifiScanLifecycle` is process-local and owned by `NetworkDaemon`. Starting a scan issues only the bounded backend trigger and returns immediately. `wifi.scan.status` polls the state machine without waiting for physical completion. `WpaEventMonitor` contributes scan-completed/scan-failed counters; result collection occurs after those counters advance. No scan worker thread and no fixed sleep is added to the IPC reactor path.

## WPA event monitor

`WpaEventMonitor` owns the existing worker `std::thread`, an atomic running flag, a mutex and a snapshot. The worker owns only wpa_supplicant event reception and L2 fact publication. It does not start udhcpc, mutate routes, or write DNS.

When an L2 CONNECTED/DISCONNECTED fact arrives, the worker invokes the thread-safe `WifiManager`, which owns the Wi-Fi L2-to-DHCP lifecycle decision. `WifiManager` delegates DHCP start/stop to `NetworkControlPlane`, whose mutex serializes that worker-thread path with main-reactor lease reconciliation.

## Process thread count

The intended steady runtime model remains:

```text
1 main / IPC / reconcile reactor thread
+
1 WPA event monitor worker thread
```

No thread is created per IPC client, DHCP lease, network interface, scan, or route/DNS operation.

## Boundary rule

- IPC owns transport/session/event fan-out and does not call Linux network mechanisms directly.
- Service/Policy (`NetworkControlPlane`, `WifiManager`) owns orchestration and selection decisions.
- Platform mechanisms (`WpaCtrlClient`, `UdhcpcProcess`, `DhcpLeaseStore`, `NetworkConfigurator`, live snapshot readers) own OS/daemon interaction only.
- udhcpc callback scripts publish Lease Facts only; CI rejects callback source that contains `ifconfig`, route mutation, or `/etc/resolv.conf` writes.

`NetworkDaemon::SnapshotProvider` remains an optional dependency-injection seam for deterministic contract tests. Production construction leaves it empty, so production snapshots continue to come from `read_live_snapshot()` through the existing platform boundary.

## Follow-up verification

The brownfield adoption still requires real-target thread/file-descriptor/resource measurements, lease-reconcile timing measurements, state-observation cost, and target restart/reconnect evidence before production promotion. Host/sanitizer results are not substitutes for HIL evidence.
