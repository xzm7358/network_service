# Network IPC v1 Lifecycle Contract Extension

Status: **FROZEN for NS-IPC-111 implementation**

This document extends `NETWORK_IPC_CONTRACT_V1.md` with server lifecycle and multi-client scheduling rules required before a production consumer may keep a v1 EVENT session open for long periods.

## 1. Reactor ownership

- The process main/IPC thread owns one `poll()` reactor.
- The reactor owns the Unix-domain listen fd, signal wake fd, and all active client fds.
- The implementation MUST NOT create one worker thread per client.
- Read/write work MUST be bounded per client per reactor turn so one connection cannot monopolize the process.

## 2. Active-client bound

- The active client set MUST have an explicit finite upper bound.
- The NS-IPC-111 host-verifiable default is **8 active clients**.
- A connection accepted while the set is full is rejected and produces the `IPC_CLIENT_CAPACITY_REJECTED` diagnostic.
- This numeric default is an implementation safety ceiling, not real-target resource-budget evidence. HIL/resource measurements may tighten it but MUST NOT remove boundedness.

## 3. Protocol selection and pre-READY lifetime

The connection-scoped selector from `NETWORK_IPC_CONTRACT_V1.md` is unchanged:

1. first four octets exactly `NSP1` -> v1;
2. otherwise -> frozen v0;
3. no protocol fallback after commitment.

Before protocol selection, while a v0 request is incomplete, and while a v1 session is still awaiting HELLO/READY completion, input-idle time is bounded. The NS-IPC-111 host default remains **1000 ms**.

## 4. READY-state lifetime

After successful HELLO/READY negotiation, a v1 session MUST NOT be closed merely because no inbound frame arrives within the pre-READY idle timeout.

A READY v1 session remains valid until one of these explicit lifecycle events occurs:

- peer disconnect;
- invalid protocol/frame/session behavior;
- outbound queue overflow;
- outbound write-stall deadline;
- server shutdown/wake;
- a future explicitly versioned session-lifetime policy.

Socket existence alone still does not imply readiness; this long-lived rule applies only after READY.

## 5. Per-client flow control

- Every v1 client retains the bounded outbound queue defined by NS-IPC-109.
- While a client has queued outbound data, the reactor may suppress additional reads from that client until outbound progress is made.
- A slow/non-reading client MUST NOT prevent the reactor from accepting or servicing another client.
- Queue overflow or write stall terminates only the affected session; recovery remains reconnect -> HELLO/READY -> authoritative `network.snapshot` rebase.

## 6. Shutdown

The signal wake pipe MUST interrupt the reactor. On reactor shutdown, all active client fds are closed deterministically before the server run loop returns.

## 7. Executable lifecycle evidence

`tests/ipc_v1_lifecycle_test.py` freezes the following host-level invariants:

1. two simultaneous v1 clients independently complete HELLO/READY;
2. a second client is not blocked behind an already READY first client;
3. READY sessions remain usable after more than the old one-second read-idle interval;
4. the active-client ceiling rejects an additional connection explicitly;
5. capacity becomes reusable after a client disconnects;
6. signal-driven shutdown exits with active READY sessions still connected.

The lifecycle regression is required in strict CI and under ASan/UBSan. Real embedded-target thread count, fd count, RSS, latency, and restart/reconnect measurements remain separate HIL evidence.
