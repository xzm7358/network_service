# NS-IPC-111 — Long-lived v1 Session Lifecycle / Multi-client Poll Loop

**Status**: IMPLEMENTED — verification pending

## Goal

Remove the final host-side server lifecycle constraints that prevent SmartControl from becoming a long-lived v1 EVENT consumer: accepted-client monopolization of the server loop and the inherited one-second READY-session read-idle timeout.

## Baseline

- `main`: `7a1ae26854fa106ad2c0785a25ccab61bdc28928`
- NS-IPC-109 merged
- post-merge EEP Product CI run #52 green

## Deliverables

- single-threaded multi-client `poll()` reactor in `NetworkIpcServer`
- bounded active-client set; host safety default = 8
- non-blocking listen/accepted sockets
- per-client protocol selection, v1 decoder/session state and bounded outbound queue
- bounded read/write work per reactor turn
- no arbitrary input-idle expiration after READY
- pre-protocol/v0/pre-READY input deadline retained
- outbound-pending flow control prevents a slow reader from driving unlimited request ingestion
- deterministic close of all active sessions on signal wake/shutdown
- `tests/ipc_v1_lifecycle_test.py`
- strict + ASan/UBSan lifecycle CI gates
- `NETWORK_IPC_LIFECYCLE_V1.md` lifecycle contract extension
- updated `thread-model.md`

## Exit criteria

- two simultaneous v1 sessions complete HELLO/READY independently
- second session is not delayed behind a READY first session
- READY session remains usable after >1 second idle
- active client set never exceeds the configured bound
- rejected capacity is reusable after disconnect
- shutdown exits with active clients without hanging
- NS-IPC-109 slow-reader semantics remain green
- full v1 contract, EVENT, rebase, v0/v1 coexistence and v0 stalled-client regressions remain green
- governance, strict build, ASan/UBSan and static analysis green

## Non-goals

- no dynamic network-state EVENT producer
- no SmartControl production migration
- no per-client threads
- no v0 removal
- no real-target HIL/resource-budget claim
