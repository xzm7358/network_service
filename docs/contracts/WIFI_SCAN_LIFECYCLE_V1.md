# Wi-Fi Scan Lifecycle v1 Addendum

Status: **FROZEN for NS-RC-001**

This addendum extends the frozen Network IPC v1 business method surface without changing the v1 frame format, request correlation, event sequencing, reconnect, or snapshot-rebase rules.

## Goal

A physical Wi-Fi scan MUST NOT keep a SmartControl/LVGL synchronous IPC call blocked for the physical scan duration.

## Methods

### `wifi.scan.start`

REQUEST params are `{}`.

A successful start returns promptly with HTTP-like status `202`:

```json
{
  "requestId": 1,
  "status": 202,
  "result": {
    "scanId": 7,
    "state": "scanning",
    "error": "",
    "results": {"count": 0, "aps": []}
  }
}
```

Rules:

1. At most one physical scan is owned by NetworkService at a time.
2. A repeated `wifi.scan.start` while state is `scanning` is idempotent: it returns the same `scanId` and MUST NOT issue a second physical scan.
3. Starting after `ready` or `failed` allocates a new monotonically increasing process-local `scanId`.
4. Immediate backend rejection returns a correlated non-2xx RESPONSE; no SmartControl fallback to `wpa_cli` is permitted.

### `wifi.scan.status`

REQUEST params are `{}`. It never waits for the physical scan to finish.

Successful RESPONSE uses status `200` and one of four states:

- `idle`: no scan has been started in this process.
- `scanning`: a physical scan is in flight.
- `ready`: `results` contains the authoritative scan result owned by NetworkService.
- `failed`: `error` is non-empty; `timeout` is the stable timeout reason.

`results` always has the shape `{ "count": N, "aps": [...] }`; before `ready`, it is empty.

## Completion ownership

NetworkService observes `CTRL-EVENT-SCAN-STARTED`, `CTRL-EVENT-SCAN-RESULTS`, and `CTRL-EVENT-SCAN-FAILED` through its existing `WpaEventMonitor`. The scan lifecycle itself owns no thread and performs no fixed sleep. Result collection occurs only after the monitor observes scan completion.

Every observed WPA event carries a process-local monotonic sequence marker. A terminal scan event is accepted only when its sequence is later than a `SCAN-STARTED` marker that is itself later than the baseline captured before the current start command. This generation fence prevents a late result from a timed-out scan from completing a subsequent scan generation.

The current implementation timeout is 5000 ms. Timeout handling is bounded and explicit; increasing SmartControl's synchronous I/O timeout is not an allowed substitute.

## Compatibility boundary

Legacy `wifi.scan` remains temporarily available for brownfield compatibility and retains its synchronous behavior. New/updated SmartControl code MUST migrate to `wifi.scan.start` + `wifi.scan.status`. Removal of legacy `wifi.scan` requires consumer migration and joint RC evidence.
