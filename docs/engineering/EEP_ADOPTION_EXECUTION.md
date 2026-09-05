# EEP Brownfield Adoption Execution Record

## Baseline

- Repository: `xzm7358/network_service`
- Original brownfield baseline: `0b415f57c9bf2519645099513f7b73c18a174e8b`
- EEP release: `1.20.0`

## NS-ADOPT-001 production adoption

`NS-ADOPT-001` was applied through product pull request #1 and merged to `main` as `f89777da1bd3cbc194a72209f8959238a851332c` after product CI passed:

- adoption metadata/hash-lock verification;
- product IPC v0 source-contract verification;
- architecture-boundary verification;
- strict host build with `-Wall -Wextra -Wpedantic -Werror`;
- ASan + UBSan build and non-mutating CLI smoke.

The adoption intentionally did not claim EEP Network IPC v1 compatibility or real target/HIL completion.

## NS-FIX-007 runtime follow-up

`NS-FIX-007` is a separate runtime safety change created only after the adoption CI gate passed. It adds:

- a 1000 ms accepted-client idle/progress bound;
- wake-pipe participation while waiting on a client so SIGTERM/SIGINT can interrupt a stalled session;
- explicit close of requests exceeding the observed 64 KiB v0 bound;
- deterministic stalled-client and shutdown regression coverage;
- sanitizer regression execution and Clang static analysis in product CI.

The frozen v0 source contract remains immutable evidence of the original baseline. The bounded-read delta is documented separately and does not claim Network IPC v1 compatibility.

## Remaining production evidence debt

The following remain outside the scope of these two host-side changes and must not be claimed as complete:

- real wall-panel target/HIL evidence;
- target resource budgets and latency measurements;
- v0 → EEP Network IPC v1 migration;
- removal of the SmartControl-specific build output path;
- production Agent Runtime/provider evidence.

## Gate rule

A runtime follow-up is eligible to merge only when its product CI, including the deterministic regression that reproduces the original failure mode, is green. GitHub Actions/PR history is the authoritative evidence for that merge gate.
