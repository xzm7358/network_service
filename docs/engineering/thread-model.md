# Thread Model

## Main / IPC reactor thread

The process main thread owns one bounded `poll()` reactor for the Unix-domain listener, the signal wake pipe, and all accepted IPC client file descriptors.

The reactor is single-threaded: no per-client worker thread is created. Each active connection owns protocol-selection state, v1 decoder/session state when applicable, and a bounded outbound queue. Read and write work is budgeted per client per reactor turn so one client cannot monopolize the process.

Protocol/lifecycle rules:

- active client count is bounded by an implementation safety ceiling;
- accepted sockets are non-blocking;
- pre-protocol, incomplete v0, and pre-READY v1 traffic retain a bounded input-idle deadline;
- after successful HELLO/READY, a v1 session has no arbitrary read-idle timeout;
- queued outbound data suppresses further reads for that client until progress is made, preserving bounded backpressure;
- write-stall and queue-overflow terminate only the affected session;
- the signal wake pipe interrupts the reactor, which closes all active clients during shutdown.

The host-side client-count/deadline ceilings are implementation safety defaults, not real embedded-target resource evidence.

## WPA event monitor

`WpaEventMonitor` owns a worker `std::thread`, an atomic running flag, a mutex and a snapshot. The worker updates WPA event state; readers obtain a synchronized snapshot.

## Boundary rule

Service and IPC code may request operations through platform/backend functions. Mechanism execution (`wpa_cli`, `udhcpc`, `ifconfig`, route/DNS mutation) belongs in the platform/backend layer, not in product UI/client code.

## Follow-up verification

The brownfield adoption still requires real-target thread/file-descriptor/resource measurements and target restart/reconnect evidence before production promotion. The reactor lifecycle is host/sanitizer verified but is not a substitute for HIL evidence.
