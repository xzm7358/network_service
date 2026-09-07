# SSD20x RC Evidence Workflow

This workflow is the final target-side evidence path for the NetworkService + SmartControl release candidate. It complements, and does not replace, the host IPC contract and cross-repository AF_UNIX E2E gates.

## Release rule

Host CI success is necessary but not sufficient for SSD20x release promotion.

Final RC proof requires all of the following:

1. a real SSD20x/smart-panel build produced by the actual product SDK/superbuild;
2. the deployed NetworkService and SmartControl source revisions recorded in `provenance.env`;
3. the deployed NetworkService binary hash recorded by the collector;
4. real target IPC v1 READY/snapshot evidence;
5. at least three physical `wifi.scan.start` -> `wifi.scan.status` cycles reaching `ready`;
6. at least two real service restart -> READY recovery cycles;
7. NetworkService `/proc` RSS/HWM/thread/FD samples;
8. SmartControl process samples when that process is present;
9. operator assertions that LVGL remained responsive during scan and recovered after restart;
10. a reviewed thresholds JSON file with every required limit frozen to a positive integer;
11. `validate_ssd20x_evidence.py` returning `RC_PROVEN`.

No host result may be substituted for items 1-9.

## Assets

- `tools/rc/network_service_rc_probe.cpp`
  - small C++17 target-side NSP1 client;
  - reuses the production IPC v1 codec;
  - validates READY identity/capabilities and requestId correlation;
  - measures monotonic READY, snapshot, physical scan, and recovery timing.
- `tools/rc/ssd20x_collect.sh`
  - BusyBox-friendly target collector;
  - uses `/proc`, `pidof`/`ps`, `sha256sum`, and the RC probe;
  - requires no Python or `jq` on the wall panel.
- `docs/contracts/ssd20x-rc-evidence-v1.json`
  - machine-readable evidence-bundle contract.
- `tools/rc/validate_ssd20x_evidence.py`
  - host-side fail-closed validator.
- `docs/contracts/ssd20x-rc-thresholds.example.json`
  - intentionally unfrozen threshold template; `null` values are rejected for final proof.

## 1. Build with the real SSD20x toolchain

Do not use an arbitrary Ubuntu ARM compiler as target proof. Record the exact compiler/toolchain/sysroot used by the product superbuild.

The NetworkService repository can expose the probe without enabling the full test suite:

```sh
cmake -S network_service -B build/network-service-ssd20x \
  -DCMAKE_TOOLCHAIN_FILE=/path/to/real/ssd20x-toolchain.cmake \
  -DBUILD_TESTING=OFF \
  -DNETWORK_SERVICE_BUILD_RC_PROBE=ON \
  -DCMAKE_INSTALL_PREFIX=/staging/dnake
cmake --build build/network-service-ssd20x --parallel
cmake --install build/network-service-ssd20x
```

This installs both `network_service` and `network_service_rc_probe` when the RC-probe option is enabled.

SmartControl must be built from its real superbuild because its SSD20x target depends on vendor MI libraries, FFmpeg/platform libraries, and the project-level `Components/hardware/ssd20x` dependency graph.

## 2. Create build provenance

Deploy a plain key/value file with the binaries. Every key below is required:

```text
network_service_revision=0123456789abcdef0123456789abcdef01234567
smartcontrol_revision=89abcdef0123456789abcdef0123456789abcdef
compiler_id=arm-linux-gnueabihf-g++ <exact version>
toolchain_id=<reviewed SDK/toolchain identifier>
sysroot_id=<reviewed sysroot/rootfs identifier>
build_id=<CI/build/release identifier>
```

Both revisions must be full 40-hex commit SHAs. The collector does not infer these from filenames.

## 3. Run the physical HIL collector

Run while an operator can observe the LVGL UI. Example:

```sh
export RC_OPERATOR='operator-name'
export RC_UI_SCAN_RESULT=pass
export RC_UI_RESTART_RESULT=pass
export RC_NOTES='Observed Settings/Wi-Fi page during three scans and two service restarts.'

/dnake/bin/ssd20x_collect.sh \
  --out /tmp/network-rc-evidence \
  --provenance /dnake/data/rc-build-provenance.env \
  --probe /dnake/bin/network_service_rc_probe \
  --service-cmd /etc/init.d/S40network_service \
  --network-bin /dnake/bin/network_service
```

The collector intentionally fails if NetworkService is absent, required provenance is missing, a scan does not reach `ready`, restart does not recover READY, or required target tooling is unavailable.

If the UI was not actually observed, leave the operator values as `UNRECORDED`. This allows raw evidence capture, but final validation will not return `RC_PROVEN`.

## 4. Copy the evidence bundle off the device

Preserve the directory as a unit. At minimum it contains:

- manifest/provenance/operator metadata;
- deployed binary hashes;
- raw READY and authoritative snapshot envelopes;
- raw start/final envelopes for every scan;
- raw READY envelope for every restart;
- scan/restart timing CSVs;
- process resource samples.

Do not edit timing/resource CSVs after capture. If operator assertions need correction, record why in `notes` and retain the original bundle in the release evidence archive.

## 5. Validate structure first

```sh
python3 tools/rc/validate_ssd20x_evidence.py \
  /path/to/network-rc-evidence \
  --structure-only \
  --json-out /path/to/structure-result.json
```

A successful structural check reports:

```text
EVIDENCE_COMPLETE_THRESHOLDS_UNFROZEN
```

That status is **not release approval**.

Running without either `--structure-only` or `--thresholds` intentionally exits non-zero with the same status. This prevents incomplete evidence from silently becoming an RC pass.

## 6. Freeze thresholds through review

Copy `ssd20x-rc-thresholds.example.json` and replace every `null` with an approved positive integer:

- `maxReadyLatencyMs`
- `maxScanStartLatencyMs`
- `maxScanCompletionMs`
- `maxRestartToReadyMs`
- `maxNetworkServiceRssKb`
- `maxNetworkServiceThreads`
- `maxNetworkServiceFd`

Thresholds are product/release requirements, not values to be reverse-engineered from one favorable sample. Review and version the threshold file with the RC decision record.

## 7. Final validation

```sh
python3 tools/rc/validate_ssd20x_evidence.py \
  /path/to/network-rc-evidence \
  --thresholds /path/to/reviewed-ssd20x-rc-thresholds.json \
  --json-out /path/to/final-rc-result.json
```

Terminal outcomes:

- `RC_PROVEN` — evidence, operator assertions, and all frozen limits pass;
- `RC_FAILED` — structurally valid evidence exceeds a limit or operator HIL assertion is not `pass`;
- `EVIDENCE_INVALID` — required files/fields/semantics are missing or malformed;
- `EVIDENCE_COMPLETE_THRESHOLDS_UNFROZEN` — evidence is structurally complete but no reviewed limits were supplied.

Only `RC_PROVEN` is a final SSD20x release-gate pass.

## Evidence boundary

This harness does not claim that host CI is target validation, and it does not define performance budgets. It makes the target measurement process deterministic, auditable, and machine-checkable so the remaining release decision depends on real hardware data rather than ad-hoc logs.
