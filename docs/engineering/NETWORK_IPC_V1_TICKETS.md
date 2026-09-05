# Network IPC v1 — Executable Tickets

Status: Step 2.1 execution backlog

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

**Status**: DONE (first tranche behavior implemented; promotion to required CI remains NS-IPC-110)

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
- non-zero reserved flags rejected
- oversized payload rejected
- partial frame delivery accepted when complete frame eventually arrives

**Exit criteria**
- Test is deterministic and uses only a temporary Unix socket/config directory.
- Test can be run against a built `network_service` binary.
- First-tranche protocol behavior is implemented; the full test remains non-required until NS-IPC-110 promotes the gate.

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
- The current server accept loop remains single-client/serial and the accepted-client read path still inherits the bounded idle timeout introduced for v0 safety. This is acceptable for the Step 2.1 handshake tranche but MUST be revisited before EVENT delivery/long-lived production sessions are promoted.

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

**Goal**: add server event envelopes with deterministic ordering metadata.

**Deliverables**
- EVENT message support
- generation lifecycle
- per-generation monotonic seq
- subscription surface

**Exit criteria**
- monotonic seq contract tests
- reconnect creates a new session and does not claim old-session continuity

---

## NS-IPC-108 — Reconnect / snapshot rebase

**Goal**: make consumer recovery deterministic across service/client restart and event gaps.

**Deliverables**
- authoritative snapshot rebase flow
- gap/generation-change behavior
- restart/reconnect integration tests

**Exit criteria**
- server restart, client restart, and sequence-gap tests green
- no stale incremental state survives rebase

---

## NS-IPC-109 — Bounded outbound backpressure

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

**Status**: NEXT

**Goal**: make v1 wire compatibility a product release gate.

**Prerequisites**
- NS-IPC-103 through NS-IPC-106 complete

**Deliverables**
- `.github/workflows/eep-ci.yml` invokes `tests/ipc_v1_contract_test.py`

**Exit criteria**
- strict host build green
- ASan/UBSan green
- v0 regression green
- v1 contract test green
- static analysis green
