# EEP Brownfield Adoption Execution Record

## Baseline

- Repository: `xzm7358/network_service`
- Baseline branch: `main`
- Baseline commit: `0b415f57c9bf2519645099513f7b73c18a174e8b`
- EEP release: `1.20.0`

## Change class

Governance/documentation/CI adoption only. This change does not intentionally modify runtime network mutation logic.

## Prepared changes

- `.eep` brownfield adoption metadata and hash lock;
- current product IPC v0 source contract freeze;
- governed v0 → EEP Network IPC v1 migration plan;
- README and eth0 policy reconciliation with executable explicit-apply behavior;
- product-owned GitHub Actions workflow;
- adoption metadata, IPC contract-surface and architecture-boundary verifiers;
- C++/thread/resource/deviation engineering profile documents.

## Local evidence

Before remote application, the staged adoption assets passed:

- adoption metadata/hash-lock verifier;
- IPC source-contract verifier self-test;
- architecture-boundary verifier self-test;
- JSON syntax validation.

## Remote execution state

GitHub write permission was restored and branch `eep/ns-adopt-001` was created from the immutable baseline commit. The adoption changes are being applied to that branch. Product PR/CI evidence is not claimed until the pull request exists and GitHub Actions completes successfully.

## Evidence not yet available

The following MUST NOT be treated as PASS until the product pull request/target environment executes:

- product GitHub Actions workflow;
- strict host build against the complete product source tree;
- ASan/UBSan product build/smoke;
- full IPC source-contract comparison against repository source in CI;
- real wall-panel target/HIL evidence;
- production Agent Runtime/provider evidence.

## Gate rule

`NS-FIX-007` remains a separate runtime change and MUST NOT be applied or merged until `NS-ADOPT-001` product CI is green.
