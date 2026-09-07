# Resource Budget

Production limits are not yet evidence-backed and therefore remain **TBD**, not invented defaults.

The real target verification must record at minimum:

- peak RSS and steady-state RSS growth;
- thread count;
- file descriptor count;
- IPC request latency;
- READY latency;
- physical `wifi.scan.start` request latency and scan-completion duration;
- restart-to-READY recovery behavior;
- SmartControl resource samples when available;
- operator-observed LVGL responsiveness during physical scan and restart.

The reproducible collection/validation path is defined in
[`ssd20x-rc-evidence.md`](ssd20x-rc-evidence.md) and the machine-readable
bundle contract is `docs/contracts/ssd20x-rc-evidence-v1.json`.

The repository deliberately does **not** freeze numerical limits here. Final RC
proof requires a reviewed thresholds JSON file supplied to
`tools/rc/validate_ssd20x_evidence.py`; without it the validator reports
`EVIDENCE_COMPLETE_THRESHOLDS_UNFROZEN` and final validation remains non-zero.

Until a real wall-panel bundle passes with reviewed thresholds, this file and
host CI results must not be used as production target evidence.
