# NetworkService IPC Contract v1

Status: **FROZEN for Step 2.1 implementation**

This document defines the product-owned Network IPC v1 wire contract for `network_service`. The immutable brownfield v0 source contract remains in `NETWORK_IPC_SOURCE_CONTRACT_V0.md`.

## 1. Transport

- Unix domain socket: `AF_UNIX` / `SOCK_STREAM`.
- Default socket path remains `/tmp/smart_hmi_network.sock` during migration.
- v1 is a persistent session protocol; a connection may carry multiple frames.
- v0 newline-delimited JSON remains a separate compatibility path during the migration window. A v0 message MUST NOT be interpreted as a malformed v1 frame and vice versa.

## 2. Frame format

Every v1 frame starts with a fixed 12-byte header followed by exactly `payloadLength` bytes of UTF-8 JSON.

| Offset | Size | Field | Encoding |
|---:|---:|---|---|
| 0 | 4 | magic | ASCII `NSP1` |
| 4 | 1 | version | unsigned integer, MUST be `1` |
| 5 | 1 | type | message type enum |
| 6 | 2 | flags | unsigned big-endian, MUST be `0` in v1 |
| 8 | 4 | payloadLength | unsigned big-endian JSON byte length |

Maximum payload length: **65536 bytes**.

Receivers MUST handle partial header reads and partial payload reads. A frame with an invalid magic, unsupported version, non-zero reserved flags, oversized payload, truncated payload, or invalid UTF-8/JSON is invalid and MUST NOT reach the NetworkDaemon command dispatcher.

## 3. Message types

| Value | Name | Direction |
|---:|---|---|
| 1 | HELLO | client -> server |
| 2 | READY | server -> client |
| 3 | REQUEST | client -> server |
| 4 | RESPONSE | server -> client |
| 5 | EVENT | server -> client |
| 6 | ERROR | both, when a session-level error can be reported safely |

Unknown message types are invalid for v1.

## 4. HELLO / READY negotiation

The first valid v1 frame sent by a client MUST be `HELLO`.

HELLO payload:

```json
{
  "minVersion": 1,
  "maxVersion": 1,
  "client": "smartcontrol",
  "capabilities": []
}
```

The server MUST reject a session whose advertised range does not include version 1.

After successful negotiation the server sends READY:

```json
{
  "version": 1,
  "service": "network_service",
  "sessionId": "opaque-session-id",
  "generation": 1,
  "capabilities": ["request-response", "events", "snapshot-rebase"]
}
```

A client MUST NOT send REQUEST frames before READY. Socket existence alone is not semantic readiness.

## 5. REQUEST / RESPONSE correlation

`requestId` is an unsigned 64-bit logical identifier encoded as a JSON integer. It MUST be non-zero and unique among outstanding requests on the session.

REQUEST payload:

```json
{
  "requestId": 42,
  "method": "network.ping",
  "params": {}
}
```

RESPONSE success payload:

```json
{
  "requestId": 42,
  "status": 200,
  "result": {}
}
```

RESPONSE error payload:

```json
{
  "requestId": 42,
  "status": 404,
  "error": {
    "code": "METHOD_NOT_FOUND",
    "message": "unknown method"
  }
}
```

The server MUST echo the exact `requestId` in the corresponding RESPONSE. Response order is not a contract; correlation is by `requestId`.

## 6. Event model

EVENT payloads are server-originated and MUST carry both `generation` and monotonically increasing `seq` within that generation.

```json
{
  "event": "network.state.changed",
  "generation": 1,
  "seq": 7,
  "payload": {}
}
```

`generation` is owned by the server process. EVENT sequence allocation is also server-owned: `seq` MUST strictly increase for every encoded EVENT in the same generation, including across client session reconnects while the server process remains in that generation. A reconnect still creates a new `sessionId`, and clients MUST NOT infer delivery continuity merely because the generation is unchanged.

### 6.1 Initial subscription surface

A client opts into the v1 event surface with an explicit REQUEST after READY:

```json
{
  "requestId": 100,
  "method": "network.events.subscribe",
  "params": {}
}
```

The successful RESPONSE is:

```json
{
  "requestId": 100,
  "status": 200,
  "result": {
    "subscribed": true
  }
}
```

On the first successful subscription in a session, the server sends one control EVENT after the correlated RESPONSE:

```json
{
  "event": "network.events.subscribed",
  "generation": 1,
  "seq": 1,
  "payload": {}
}
```

The subscription REQUEST is idempotent within a session. Repeating it returns a successful RESPONSE but MUST NOT emit a second `network.events.subscribed` control EVENT for that same session.

This initial control EVENT establishes executable generation/sequence semantics only. It is **not** an authoritative network-state snapshot. Continuous dynamic state EVENT production remains a separate promotion decision even though the bounded outbound transport is now available.

A sequence gap, generation change, or reconnect invalidates the consumer's incremental projection. The consumer MUST perform the authoritative snapshot rebase defined below before trusting incremental events again.

## 7. Reconnect and authoritative snapshot rebase

- A transport disconnect terminates the session and all outstanding requests.
- `sessionId` is not reusable across server restart/reconnect.
- After reconnect the client performs HELLO -> READY again.
- Event subscription is session-scoped and MUST be repeated after reconnect.
- Reconnect, generation change, or EVENT sequence gap invalidates the current incremental projection.
- An invalidated consumer MUST obtain a fresh authoritative snapshot before incremental EVENTs are trusted again.

### 7.1 `network.snapshot`

After READY, the client requests an authoritative rebase point with:

```json
{
  "requestId": 200,
  "method": "network.snapshot",
  "params": {}
}
```

Successful RESPONSE:

```json
{
  "requestId": 200,
  "status": 200,
  "result": {
    "generation": 1,
    "snapshotSeq": 7,
    "snapshot": {}
  }
}
```

Rules:

1. `result.generation` MUST equal the server generation advertised by READY for that session.
2. `snapshot` is the authoritative live NetworkService snapshot captured for the rebase.
3. `snapshotSeq` is the last EVENT sequence successfully allocated in that generation at the snapshot rebase point. It is `0` when the generation has not emitted any EVENT yet.
4. The tuple `(generation, snapshotSeq, snapshot)` establishes the consumer's new baseline.
5. EVENTs from a different generation MUST invalidate the baseline and require another snapshot rebase.
6. EVENTs with `seq <= snapshotSeq` are stale/already covered by the authoritative baseline and MUST NOT advance the projection.
7. The first accepted incremental EVENT after a baseline MUST have `seq == snapshotSeq + 1`; every later accepted EVENT MUST likewise be exactly the next sequence.
8. A forward sequence jump (`seq > lastAcceptedSeq + 1`) is a gap and MUST invalidate the baseline and trigger a new snapshot rebase.
9. A reconnect always invalidates the previous session's projection even when READY reports the same server generation. A new session MUST establish a fresh snapshot baseline before relying on incremental delivery.

## 8. Backpressure and bounds

- Maximum v1 payload: 64 KiB.
- Readers MUST be bounded and interruptible.
- Every v1 connection MUST use a bounded outbound queue; an implementation MUST NOT accumulate unbounded RESPONSE/EVENT bytes for a slow client.
- Queue accounting MUST bound both logical frame count and encoded bytes, and partial writes MUST reduce the accounted byte count exactly.
- The current host-verifiable default ceiling is **32 queued frames** and **4 × maximum encoded v1 frame bytes**. These are implementation safety defaults, not real-target resource-budget evidence. Real-target evidence may tighten these values but MUST NOT remove boundedness.
- Queue overflow MUST NOT silently drop an individual RESPONSE or EVENT. The session is terminated as overloaded; incremental delivery continuity is invalidated.
- v1 socket writes MUST be non-blocking or otherwise deadline-bounded. The current implementation write-stall deadline is **1000 ms**.
- A write stall or queue overflow terminates the v1 session. Recovery is a new connection followed by HELLO -> READY -> authoritative `network.snapshot` rebase before incremental EVENTs are trusted.
- Service shutdown/wake MUST interrupt an outbound wait; a slow client MUST NOT indefinitely delay process termination.
- Overload/slow-client termination MUST emit an explicit diagnostic suitable for regression evidence.

The v0 compatibility path is not redefined by this section; the existing bounded v0 stalled-client regression remains required during migration.

The bounded writer makes future continuous EVENT delivery safe from unbounded outbound memory growth, but NS-IPC-109 by itself does **not** introduce a new dynamic network-state EVENT producer or remove the current single-client/serial accept-loop constraint.

## 9. Compatibility and protocol-selection rule

During the bounded migration window, v0 and v1 share the same Unix-domain socket. Protocol selection is connection-scoped and is frozen as follows:

1. Read enough initial octets to decide whether the connection prefix is the four-byte ASCII magic `NSP1`.
2. If the first four octets are exactly `NSP1`, the connection is committed to the v1 framed-session path for its lifetime.
3. Otherwise the connection is committed to the frozen v0 newline-delimited JSON path for its lifetime.
4. Once a connection is committed to v1, any malformed v1 header/frame is rejected as v1 and MUST NOT fall through to the v0 JSON parser.
5. Once a connection is committed to v0, occurrences of the string `NSP1` later in the JSON payload have no protocol-selection meaning.
6. Protocol selection MUST NOT inspect JSON fields such as `method`, `version`, or payload shape. It is a transport-prefix discriminator, not heuristic field sniffing.

Migration rules:

- Existing v0 consumers remain supported only through the explicit v0 path.
- New consumers MUST use v1.
- `tools/networkctl.py` remains intentionally v0 during this bounded migration window and serves as a compatibility sentinel; it MUST NOT silently auto-negotiate or fall back between protocols.
- v0 removal requires consumer migration, CI evidence, and real-target restart/reconnect evidence.

## 10. Executable contract tranches

The executable contract currently freezes these invariants:

1. 12-byte `NSP1` header encoding/decoding.
2. Big-endian payload length.
3. partial header/payload reads.
4. maximum payload enforcement.
5. HELLO MUST precede REQUEST.
6. supported HELLO produces READY version 1.
7. unsupported version range is rejected.
8. RESPONSE echoes non-zero uint64 `requestId`.
9. invalid magic/version/type/flags never reaches command dispatch.
10. valid v0 and valid v1 traffic remain disjoint under the frozen four-octet protocol selector.
11. first event subscription produces an EVENT whose generation matches READY and whose server-owned sequence is monotonic within the generation.
12. reconnect snapshot rebase returns READY generation plus a sequence watermark and authoritative snapshot.
13. generation changes, sequence gaps, stale EVENTs, and exact-next EVENTs have deterministic rebase-state decisions.
14. outbound queue frame/byte bounds, partial-send accounting, write-stall deadline, wake interruption, and reconnect/rebase recovery are executable regressions.

Multi-outstanding request concurrency, production SmartControl migration, dynamic network-state EVENT production, and real-target resource/HIL evidence remain subsequent work.
