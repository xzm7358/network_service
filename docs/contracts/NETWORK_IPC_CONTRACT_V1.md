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
  "capabilities": ["request-response"]
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

A sequence gap, generation change, or reconnect invalidates the consumer's incremental projection. The consumer MUST obtain an authoritative snapshot and then consume only events newer than the rebased point.

Event subscription and full event surface are Step 2.1 follow-up work; the sequencing rules are frozen here so later implementation cannot choose incompatible semantics.

## 7. Reconnect semantics

- A transport disconnect terminates the session and all outstanding requests.
- `sessionId` is not reusable across server restart/reconnect.
- After reconnect the client performs HELLO -> READY again.
- State reconciliation is authoritative snapshot rebase, then newer events.
- Clients MUST NOT assume event continuity across sessions.

## 8. Backpressure and bounds

- Maximum payload: 64 KiB.
- Readers MUST be bounded and interruptible.
- A partial/stalled client MUST NOT block service shutdown indefinitely.
- Implementations MUST bound queued outbound data; overload behavior must become explicit before EVENT delivery is enabled.

The existing v0 stalled-client regression remains required during migration.

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

## 10. Initial contract-test tranche

The first executable tranche freezes these invariants:

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

Event delivery, generation-gap reconciliation, multi-outstanding request concurrency, and explicit outbound-backpressure behavior are subsequent tranches.
