# NetworkService Product IPC Source Contract v0

This document freezes the **actual brownfield product protocol** observed at commit `0b415f57c9bf2519645099513f7b73c18a174e8b`. It is evidence of what the product implements today; it is not the target EEP Network IPC v1 contract.

## Transport and framing

- Unix domain `SOCK_STREAM`.
- Default path: `/tmp/smart_hmi_network.sock`.
- One request and one response per accepted connection.
- Request is accumulated until newline, EOF, or the observed 64 KiB threshold.
- Response is a single JSON line.
- No length prefix.

## Request model

Minimum form:

```json
{"method":"network.ping"}
```

There is no request ID or protocol version field in v0.

## Response model

Success:

```json
{"status":200,"result":{}}
```

Error:

```json
{"status":500,"error":"..."}
```

Unknown methods return status 404. An empty/missing method currently falls back to `network.snapshot`; consumers must not rely on that behavior for the target v1 protocol.

## Implemented method surface

- `network.ping`
- `network.snapshot`
- `eth.get_config`
- `eth.set_dhcp`
- `eth.set_static`
- `wpa.events`
- `wifi.scan`
- `wifi.set_enabled`
- `wifi.connect`
- `wifi.connect_saved`
- `wifi.saved_list`
- `wifi.forget`
- `wifi.autoconnect`
- `wifi.disconnect`

`network.subscribe` is declared in the public protocol header but has no matching handler in the frozen implementation and is therefore **not part of the implemented v0 behavior**.

## Known incompatibilities with EEP Network IPC v1

The platform v1 contract additionally requires framed/versioned transport semantics, readiness negotiation, request correlation, bounded backpressure, event sequencing/generation and reconnect reconciliation. Those semantics are not claimed by this v0 source contract.

Any migration must preserve this document as immutable baseline evidence rather than rewriting history to make the old implementation look v1-compatible.
