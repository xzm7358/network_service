# Network IPC v1 — Executable Tickets

Status: Step 2.1 execution backlog

## NS-IPC-101 — Freeze v1 wire contract

**Goal**: freeze transport, framing, negotiation, request correlation, event sequencing, reconnect, and compatibility semantics.

**Deliverables**
- `docs/contracts/NETWORK_IPC_CONTRACT_V1.md`

**Exit criteria**
- Contract is explicit enough to write black-box wire tests without reading implementation.
- v0 baseline remains immutable.

---

## NS-IPC-102 — Add first black-box protocol contract tests

**Goal**: create an executable RED test tranche for the v1 session contract before server integration.

**Deliverables**
- `tests/ipc_v1_contract_test.py`

**Cases**
- valid HELLO -> READY
- REQUEST before HELLO rejected
- unsupported version rejected
- valid REQUEST after READY -> RESPONSE with same requestId
- invalid magic rejected
- invalid version rejected
- non-zero reserved flags rejected
- oversized payload rejected
- partial frame delivery accepted when complete frame eventually arrives

**Exit criteria**
- Test is deterministic and uses only a temporary Unix socket/config directory.
- Test can be run against a built `network_service` binary.
- Until NS-IPC-104 is implemented, this test is expected RED and is not yet a required CI gate.

---

## NS-IPC-103 — Introduce v1 protocol codec boundary

**Goal**: remove framing/parsing responsibility from `NetworkIpcServer`.

**Deliverables**
- `src/ipc/network_ipc_v1_codec.{h,cpp}`
- frame header encode/decode
- bounded payload reader
- JSON payload validation boundary

**Constraints**
- no NetworkDaemon business behavior change
- no event implementation yet
- preserve v0 path

**Exit criteria**
- codec unit/contract tests green
- strict build, ASan/UBSan, static analysis green

---

## NS-IPC-104 — Integrate HELLO / READY session state

**Goal**: make socket readiness semantic rather than process/socket existence.

**Deliverables**
- per-connection session state
- HELLO version-range validation
- READY response with sessionId/generation
- reject REQUEST before READY

**Exit criteria**
- first tranche negotiation tests green
- stalled-client regression remains green

---

## NS-IPC-105 — Add requestId correlation

**Goal**: v1 REQUEST/RESPONSE correlation independent of response ordering.

**Deliverables**
- validate non-zero uint64 requestId
- dispatch `network.ping` through v1 request envelope
- RESPONSE echoes exact requestId
- structured v1 error envelope

**Exit criteria**
- correlation tests green
- unknown method returns `METHOD_NOT_FOUND`

---

## NS-IPC-106 — Freeze v0/v1 coexistence boundary

**Goal**: avoid heuristic protocol ambiguity during migration.

**Deliverables**
- explicit protocol-selection rule in server
- compatibility tests for existing newline-delimited v0 requests
- `tools/networkctl.py` migration decision recorded

**Exit criteria**
- all frozen v0 tests green
- all first-tranche v1 tests green
- valid v0 cannot be misclassified as v1

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
