# Network IPC v1 — Executable Tickets

Status: Step 2.1 first wire-contract, EVENT sequencing/generation, and reconnect/snapshot-rebase tranches COMPLETE; bounded outbound backpressure remains the next protocol tranche.

## NS-IPC-101 — Freeze v1 wire contract

**Status**: DONE

**Goal**: freeze transport, framing, negotiation, request correlation, event sequencing, reconnect, and compatibility semantics.

**Deliverables**
- `docs/contracts/NETWORK_IPC_CONTRACT_V1.md`

**Exit criteria**
- Contract is explicit enough to write black-box wire tests without reading implementation.
- v0 baseline remains immutable.

---

## NS-IPC-102 — Add first black-box protocol contract tests

**Status**: DONE (promoted to required CI by NS-IPC-110)

**Goal**: create an executable RED test tranche for the v1 session contract before server integration.

**Deliverables**
- `tests/ipc_v1_contract_test.py`

**Cases**
- valid HELLO -> READY
- REQUEST before HELLO rejected
- unsupported version rejected
- valid REQUEST after READY -> RESPONSE with same requestId
- invalid v1 framing rejected after the connection is committed to v1
- invalid version rejected
- invalid message type rejected
- non-zero reserved flags rejected
- oversized payload rejected
- partial frame delivery accepted when complete frame eventually arrives

**Exit criteria**
- Test is deterministic and uses only a temporary Unix socket/config directory.
- Test can be run against a built `network_service` binary.
- Full first-tranche protocol behavior is a required CI release gate.

---

## NS-IPC-103 — Introduce v1 protocol codec boundary

**Status**: DONE

**Goal**: remove binary framing responsibility from `NetworkIpcServer`.

**Deliverables**
- `src/ipc/network_ipc_v1_codec.{h,cpp}`
- frame header encode/decode
- bounded payload accumulation
- frame-level validation boundary

**Constraints**
- no NetworkDaemon business behavior change
- no event implementation yet
- preserve v0 path

**Exit criteria**
- codec unit/contract tests green
- strict build, ASan/UBSan, static analysis green

---

## NS-IPC-104 — Integrate HELLO / READY session state

**Status**: DONE

**Goal**: make socket readiness semantic rather than process/socket existence.

**Deliverables**
- per-connection session state
- HELLO JSON validation and version-range validation
- READY response with sessionId/generation
- reject REQUEST before READY
- black-box session negotiation regression

**Exit criteria**
- negotiation unit/integration tests green
- strict build and sanitizer runs green
- stalled-client v0 regression remains green
- static analysis green

**Known follow-up constraint**
- The current server accept loop remains single-client/serial and the accepted-client read path still inherits the bounded idle timeout introduced for v0 safety. This is acceptable for the Step 2.1 handshake tranche but MUST be revisited before continuous EVENT delivery/long-lived production sessions are promoted.

---

## NS-IPC-105 — Add requestId correlation

**Status**: DONE

**Goal**: v1 REQUEST/RESPONSE correlation independent of response ordering.

**Deliverables**
- validate non-zero uint64 requestId, including exact `UINT64_MAX` handling
- reject zero, negative, fractional, overflow, duplicate, and missing requestId forms
- handle protocol-level `network.ping` through the v1 request envelope
- RESPONSE echoes exact requestId
- structured correlated error envelope
- unknown method returns correlated `METHOD_NOT_FOUND`
- READY advertises `request-response` capability

**Exit criteria**
- unit correlation tests green
- black-box Unix-socket correlation tests green
- strict build green
- ASan/UBSan green
- existing v0 stalled-client regression green
- static analysis green

---

## NS-IPC-106 — Freeze v0/v1 coexistence boundary

**Status**: DONE

**Goal**: avoid heuristic protocol ambiguity during migration.

**Deliverables**
- freeze connection-scoped selector: first four octets exactly `NSP1` -> v1; otherwise -> v0
- freeze no-fallback semantics after protocol commitment
- add direct v0/v1 coexistence black-box regression
- prove fragmented/whitespace-prefixed v0 requests remain v0
- prove `NSP1` inside a v0 JSON payload has no selector meaning
- prove malformed framing after v1 commitment cannot fall through to v0
- keep `tools/networkctl.py` intentionally v0 as a compatibility sentinel
- align the migration plan with the frozen 12-byte `NSP1` header

**Exit criteria**
- frozen v0 behavior green
- v1 codec/session/correlation behavior green
- strict coexistence regression green
- sanitizer coexistence regression green
- valid v0 cannot be misclassified as v1
- a connection committed to v1 cannot fall through to v0
- existing stalled-client v0 regression remains green
- governance and static analysis green

---

## NS-IPC-107 — Event sequencing and generation

**Status**: DONE

**Goal**: add server event envelopes with deterministic ordering metadata without prematurely enabling an unbounded asynchronous event stream.

**Deliverables**
- `src/ipc/network_ipc_v1_event.{h,cpp}` server-owned `EventSequencer`
- EVENT message encoding with process generation and monotonic per-generation `seq`
- READY advertises `events`
- explicit session-scoped `network.events.subscribe` request
- first successful subscription emits one `network.events.subscribed` control EVENT after its correlated RESPONSE
- duplicate subscription is idempotent and does not emit a duplicate control EVENT
- `tests/network_ipc_v1_event_test.cpp` generation/sequence contract test
- `tests/ipc_v1_event_test.py` real Unix-socket reconnect/generation/sequence regression
- strict and ASan/UBSan CI gates for EVENT sequencing regression

**Exit criteria**
- EVENT envelope unit test green
- READY generation equals EVENT generation
- server-owned EVENT `seq` strictly increases within one generation, including across reconnect sessions
- reconnect creates a new `sessionId` and does not claim old-session delivery continuity
- existing full v1 wire contract remains green
- v0/v1 coexistence remains green
- existing v0 stalled-client regression remains green
- governance, strict build, ASan/UBSan, and static analysis green

**Boundary retained for follow-up**
- NS-IPC-107 emits only one synchronous subscription control EVENT per session and introduces no outbound EVENT queue.
- Continuous/unsolicited state EVENT delivery remains disabled until NS-IPC-109 defines bounded slow-client/backpressure behavior.
- Authoritative reconnect/snapshot rebase is now implemented by NS-IPC-108.
- The current single-client/serial accept loop and short accepted-client idle timeout remain a known production constraint for long-lived EVENT sessions.

---

## NS-IPC-108 — Reconnect / snapshot rebase

**Status**: DONE

**Goal**: make consumer recovery deterministic across reconnect, server restart, generation change, and EVENT sequence gaps.

**Deliverables**
- v1 `network.snapshot` REQUEST surface and READY `snapshot-rebase` capability
- `NetworkDaemon::snapshot_result_json()` authoritative live snapshot payload boundary
- `EventSequencer::last_sequence()` server-owned snapshot watermark
- correlated snapshot RESPONSE containing `generation`, `snapshotSeq`, and authoritative `snapshot`
- `src/ipc/network_ipc_v1_rebase.{h,cpp}` with `RebaseTracker`
- deterministic `Accept` / `IgnoreStale` / `RebaseRequired` event decisions
- reconnect, generation-change, gap, stale-event, duplicate-event, and exact-next-event semantics
- `tests/network_ipc_v1_rebase_test.cpp` rebase state/envelope contract test
- `tests/ipc_v1_rebase_test.py` real Unix-socket reconnect + server-restart regression
- strict and ASan/UBSan CI gates for reconnect/snapshot rebase

**Exit criteria**
- reconnect creates a fresh `sessionId` and requires a fresh snapshot baseline
- same-process reconnect preserves server generation and snapshot watermark covers prior EVENTs
- first accepted post-rebase EVENT is exactly `snapshotSeq + 1`
- server restart changes generation, resets snapshot watermark to `0`, and begins new-generation EVENT sequence at `1`
- generation mismatch and forward sequence gap invalidate the baseline and require rebase
- stale/duplicate EVENTs do not advance the projection
- authoritative snapshot RESPONSE preserves exact request correlation
- existing full v1 wire contract, EVENT sequencing, v0/v1 coexistence, and v0 stalled-client regressions remain green
- governance, strict build, ASan/UBSan, and static analysis green

**Boundary retained for follow-up**
- Continuous/unsolicited state EVENT delivery is still disabled.
- NS-IPC-108 introduces no outbound EVENT queue.
- Subscription/snapshot ordering for continuous delivery must be paired with the bounded queue/backpressure semantics in NS-IPC-109 so state changes cannot fall into an unobservable rebase window.

---

## NS-IPC-109 — Bounded outbound backpressure

**Status**: NEXT

**Goal**: prevent slow clients from consuming unbounded memory or blocking the service.

**Deliverables**
- bounded outbound queue
- explicit overload policy/diagnostic
- slow-reader regression

**Exit criteria**
- deterministic queue bound test
- shutdown remains interruptible

---

## NS-IPC-110 — Promote v1 contract test to required CI

**Status**: DONE

**Goal**: make v1 wire compatibility a product release gate.

**Prerequisites**
- NS-IPC-103 through NS-IPC-106 complete

**Deliverables**
- audit `tests/ipc_v1_contract_test.py` against the frozen initial tranche
- add invalid message-type coverage
- exercise exact `UINT64_MAX` requestId correlation
- `.github/workflows/eep-ci.yml` invokes the full contract test in strict-host CI
- `.github/workflows/eep-ci.yml` invokes the full contract test under ASan/UBSan
- keep v0/v1 coexistence and bounded v0 stalled-client regressions required

**Exit criteria**
- complete v1 wire contract gate green (10/10)
- strict host build green
- ASan/UBSan green
- v0/v1 coexistence regression green
- v0 stalled-client regression green
- governance green
- static analysis green
