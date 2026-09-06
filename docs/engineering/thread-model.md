# Thread Model

## Main / IPC reactor thread

The process main thread owns one bounded `poll()` reactor for the Unix-domain listener, the signal wake pipe, all accepted IPC client file descriptors, and the dynamic network-state observation cadence.

The reactor is single-threaded: no per-client worker thread and no EVENT-producer worker thread is created. Each active connection owns protocol-selection state, v1 decoder/session state when applicable, and a bounded outbound queue. Read and write work is budgeted per client per reactor turn so one client cannot monopolize the process.

Protocol/lifecycle rules:

- active client count is bounded by an implementation safety ceiling;
- accepted sockets are non-blocking;
- pre-protocol, incomplete v0, and pre-READY v1 traffic retain a bounded input-idle deadline;
- after successful HELLO/READY, a v1 session has no arbitrary read-idle timeout;
- queued outbound data suppresses further reads for that client until progress is made, preserving bounded backpressure;
- write-stall and queue-overflow terminate only the affected session;
- the signal wake pipe interrupts the reactor, which closes all active clients during shutdown.

### Reactor-owned network state observer

The same reactor owns a `NetworkStateChangeDetector` baseline and a timed observation deadline. The current host-verifiable cadence is 250 ms. When the deadline is reached, the reactor reads `NetworkDaemon::snapshot()`, compares the new authoritative snapshot with the previous semantic baseline, and allocates at most one `network.state.changed` EVENT for that observed transition.

This observer does not add a thread. Its deadline participates in the same `poll()` timeout calculation used for connection lifecycle and outbound write-stall deadlines.

EVENT fan-out remains reactor-owned:

- one state transition is encoded exactly once by the generation-global `EventSequencer`;
- the same encoded EVENT/sequence is offered to every currently subscribed healthy v1 session;
- a state transition advances the global sequence watermark even when no client is subscribed, keeping later `network.snapshot.snapshotSeq` truthful;
- globally sequenced subscription control EVENTs are likewise visible to all already-subscribed healthy sessions so one client's subscription cannot create an invisible sequence hole for another;
- a slow/overloaded recipient can be closed by its existing bounded outbound policy without preventing delivery attempts to other healthy recipients.

The 250 ms observation cadence and host-side client-count/deadline ceilings are implementation safety defaults, not real embedded-target timing or resource evidence.

## WPA event monitor

`WpaEventMonitor` owns the existing worker `std::thread`, an atomic running flag, a mutex and a snapshot. The worker updates WPA event state; readers obtain a synchronized snapshot. NS-IPC-112 does not add or repurpose a WPA-monitor thread for IPC EVENT production.

## Boundary rule

Service and IPC code may request operations through platform/backend functions. Mechanism execution (`wpa_cli`, `udhcpc`, `ifconfig`, route/DNS mutation) belongs in the platform/backend layer, not in product UI/client code.

`NetworkDaemon::SnapshotProvider` is an optional dependency-injection seam for deterministic contract tests. Production construction leaves it empty, so production snapshots continue to come from `read_live_snapshot()` through the existing platform boundary.

## Follow-up verification

The brownfield adoption still requires real-target thread/file-descriptor/resource measurements, state-observation cost/timing measurements, and target restart/reconnect evidence before production promotion. The reactor lifecycle and dynamic EVENT producer are host/sanitizer verified but are not substitutes for HIL evidence.
