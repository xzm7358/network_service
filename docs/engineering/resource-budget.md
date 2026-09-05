# Resource Budget

Production limits are not yet evidence-backed and therefore remain **TBD**, not invented defaults.

The real target verification must record at minimum:

- peak RSS and steady-state RSS growth;
- thread count;
- file descriptor count;
- IPC request latency (including P95);
- startup-to-READY latency after the v1 readiness contract exists;
- queue/backpressure limits after event delivery is introduced;
- restart/reconnect recovery behavior.

Until real wall-panel measurements are captured, this file must not be used as production target evidence.
