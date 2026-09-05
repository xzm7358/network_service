# NS-IPC-111 — Long-lived v1 Session Lifecycle / Multi-client Poll Loop

**Status**: DONE

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

- two simultaneous v1 sessions complete HELLO/READY independently — PASS
- second session is not delayed behind a READY first session — PASS
- READY session remains usable after >1 second idle — PASS
- active client set never exceeds the configured bound — PASS
- rejected capacity is reusable after disconnect — PASS
- shutdown exits with active clients without hanging — PASS
- NS-IPC-109 slow-reader semantics remain green — PASS
- full v1 contract, EVENT, rebase, v0/v1 coexistence and v0 stalled-client regressions remain green — PASS
- governance, strict build, ASan/UBSan and static analysis green — PASS

## Verification evidence

Implementation head before this status-only update: `f3d56d4f218fdbb5b508dff340c01ba08f2339a7`.

EEP Product CI run #53 (`33967148673`): SUCCESS.

- governance/adoption metadata: PASS
- strict host build: PASS
- v1 codec/session/full-contract regressions: PASS
- EVENT sequencing: PASS
- reconnect/snapshot rebase: PASS
- bounded outbound backpressure: PASS
- long-lived multi-client lifecycle regression: PASS
- ASan/UBSan lifecycle regression: PASS
- v0/v1 coexistence and v0 stalled-client regressions: PASS
- Clang Static Analyzer: PASS

## Non-goals

- no dynamic network-state EVENT producer
- no SmartControl production migration
- no per-client threads
- no v0 removal
- no real-target HIL/resource-budget claim
