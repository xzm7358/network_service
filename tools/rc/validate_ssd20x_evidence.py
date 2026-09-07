#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import json
import re
import sys
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_CONTRACT = ROOT / "docs" / "contracts" / "ssd20x-rc-evidence-v1.json"
SHA40 = re.compile(r"^[0-9a-fA-F]{40}$")
SHA256 = re.compile(r"^[0-9a-fA-F]{64}$")


class EvidenceError(RuntimeError):
    pass


def load_json(path: Path) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise EvidenceError(f"cannot parse JSON {path}: {exc}") from exc


def load_env(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as exc:
        raise EvidenceError(f"cannot read {path}: {exc}") from exc
    for line_no, raw in enumerate(lines, 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if "=" not in line:
            raise EvidenceError(f"{path}:{line_no}: expected key=value")
        key, value = line.split("=", 1)
        key = key.strip()
        if not key or key in values:
            raise EvidenceError(f"{path}:{line_no}: invalid/duplicate key {key!r}")
        values[key] = value.strip()
    return values


def load_csv(path: Path) -> list[dict[str, str]]:
    try:
        with path.open("r", encoding="utf-8", newline="") as handle:
            reader = csv.DictReader(handle)
            if not reader.fieldnames:
                raise EvidenceError(f"{path}: missing CSV header")
            return [dict(row) for row in reader]
    except OSError as exc:
        raise EvidenceError(f"cannot read {path}: {exc}") from exc


def require_keys(values: dict[str, str], keys: list[str], where: str) -> None:
    for key in keys:
        if key not in values or values[key] == "":
            raise EvidenceError(f"{where}: missing required key {key}")


def as_int(value: str, where: str, *, minimum: int = 0) -> int:
    try:
        parsed = int(value, 10)
    except (TypeError, ValueError) as exc:
        raise EvidenceError(f"{where}: expected integer, got {value!r}") from exc
    if parsed < minimum:
        raise EvidenceError(f"{where}: expected >= {minimum}, got {parsed}")
    return parsed


def metric_value(bundle: Path, name: str, key: str) -> int:
    values = load_env(bundle / name)
    require_keys(values, [key], name)
    return as_int(values[key], f"{name}:{key}", minimum=0)


def validate_response_json(path: Path, *, snapshot: bool = False) -> dict[str, Any]:
    doc = load_json(path)
    if not isinstance(doc, dict):
        raise EvidenceError(f"{path}: JSON root must be an object")
    if snapshot:
        if doc.get("status") != 200:
            raise EvidenceError(f"{path}: snapshot status must be 200")
        result = doc.get("result")
        if not isinstance(result, dict):
            raise EvidenceError(f"{path}: snapshot result missing")
        generation = result.get("generation")
        snapshot_seq = result.get("snapshotSeq")
        snapshot_obj = result.get("snapshot")
        if not isinstance(generation, int) or generation <= 0:
            raise EvidenceError(f"{path}: invalid snapshot generation")
        if not isinstance(snapshot_seq, int) or snapshot_seq < 0:
            raise EvidenceError(f"{path}: invalid snapshotSeq")
        if not isinstance(snapshot_obj, dict):
            raise EvidenceError(f"{path}: authoritative snapshot object missing")
    return doc


def load_thresholds(path: Path) -> dict[str, int]:
    doc = load_json(path)
    if not isinstance(doc, dict) or doc.get("schemaVersion") != 1:
        raise EvidenceError("thresholds: schemaVersion must be 1")
    required = [
        "maxReadyLatencyMs",
        "maxScanStartLatencyMs",
        "maxScanCompletionMs",
        "maxRestartToReadyMs",
        "maxNetworkServiceRssKb",
        "maxNetworkServiceThreads",
        "maxNetworkServiceFd",
    ]
    out: dict[str, int] = {}
    for key in required:
        value = doc.get(key)
        if not isinstance(value, int) or isinstance(value, bool) or value <= 0:
            raise EvidenceError(f"thresholds: {key} must be a positive integer")
        out[key] = value
    return out


def validate_bundle(bundle: Path, contract_path: Path) -> dict[str, Any]:
    contract = load_json(contract_path)
    if not isinstance(contract, dict) or contract.get("schemaVersion") != 1:
        raise EvidenceError("evidence contract schemaVersion must be 1")

    for rel in contract.get("requiredFiles", []):
        path = bundle / rel
        if not path.is_file():
            raise EvidenceError(f"missing required evidence file: {rel}")

    manifest = load_env(bundle / "manifest.env")
    provenance = load_env(bundle / "provenance.env")
    operator = load_env(bundle / "operator.env")
    require_keys(manifest, contract["requiredManifestKeys"], "manifest.env")
    require_keys(provenance, contract["requiredProvenanceKeys"], "provenance.env")
    require_keys(operator, contract["operatorAssertions"], "operator.env")

    if as_int(manifest["schema_version"], "manifest.env:schema_version", minimum=1) != 1:
        raise EvidenceError("manifest.env: unsupported schema_version")
    for key in ("network_service_revision", "smartcontrol_revision"):
        if not SHA40.fullmatch(provenance[key]):
            raise EvidenceError(f"provenance.env:{key} must be a 40-hex commit SHA")
    for key in ("network_service_binary_sha256", "rc_probe_sha256"):
        if not SHA256.fullmatch(manifest[key]):
            raise EvidenceError(f"manifest.env:{key} must be SHA-256")

    ready = load_json(bundle / "raw" / "ready.json")
    if not isinstance(ready, dict):
        raise EvidenceError("raw/ready.json: root must be object")
    if ready.get("version") != 1 or ready.get("service") != "network_service":
        raise EvidenceError("raw/ready.json: invalid READY identity")
    if not isinstance(ready.get("generation"), int) or ready["generation"] <= 0:
        raise EvidenceError("raw/ready.json: generation must be positive")
    capabilities = ready.get("capabilities")
    if not isinstance(capabilities, list) or not all(isinstance(x, str) for x in capabilities):
        raise EvidenceError("raw/ready.json: capabilities must be string array")
    for capability in contract["requiredReadyCapabilities"]:
        if capability not in capabilities:
            raise EvidenceError(f"raw/ready.json: missing capability {capability}")

    validate_response_json(bundle / "raw" / "snapshot.json", snapshot=True)
    validate_response_json(bundle / "raw" / "post_restart_snapshot.json", snapshot=True)

    ready_latency = metric_value(bundle, "ready.metrics", "ready_latency_ms")
    snapshot_latency = metric_value(bundle, "snapshot.metrics", "snapshot_latency_ms")
    _ = metric_value(bundle, "post_restart_snapshot.metrics", "snapshot_latency_ms")

    scans = load_csv(bundle / "scan_samples.csv")
    minimum_scan = int(contract["minimumSamples"]["scan"])
    if len(scans) < minimum_scan:
        raise EvidenceError(f"scan_samples.csv: need at least {minimum_scan} rows")
    scan_start_latencies: list[int] = []
    scan_completion: list[int] = []
    seen_scan_samples: set[int] = set()
    for row in scans:
        sample = as_int(row.get("sample", ""), "scan sample", minimum=1)
        if sample in seen_scan_samples:
            raise EvidenceError("scan_samples.csv: duplicate sample index")
        seen_scan_samples.add(sample)
        as_int(row.get("scan_id", ""), f"scan {sample}:scan_id", minimum=1)
        start_ms = as_int(row.get("start_latency_ms", ""), f"scan {sample}:start_latency_ms")
        completion_ms = as_int(row.get("completion_ms", ""), f"scan {sample}:completion_ms")
        if completion_ms < start_ms:
            raise EvidenceError(f"scan {sample}: completion_ms < start_latency_ms")
        if row.get("final_state") != contract["requiredScanTerminalState"]:
            raise EvidenceError(f"scan {sample}: final_state is not ready")
        for suffix in ("start.json", "final.json"):
            raw = bundle / "raw" / f"scan_{sample}.{suffix}"
            if not raw.is_file():
                raise EvidenceError(f"missing raw scan evidence: {raw.relative_to(bundle)}")
            validate_response_json(raw)
        scan_start_latencies.append(start_ms)
        scan_completion.append(completion_ms)

    restarts = load_csv(bundle / "restart_samples.csv")
    minimum_restart = int(contract["minimumSamples"]["restart"])
    if len(restarts) < minimum_restart:
        raise EvidenceError(f"restart_samples.csv: need at least {minimum_restart} rows")
    restart_to_ready: list[int] = []
    for row in restarts:
        sample = as_int(row.get("sample", ""), "restart sample", minimum=1)
        total = as_int(row.get("restart_to_ready_ms", ""), f"restart {sample}:restart_to_ready_ms")
        as_int(row.get("probe_wait_ready_ms", ""), f"restart {sample}:probe_wait_ready_ms")
        raw = bundle / "raw" / f"restart_{sample}.ready.json"
        if not raw.is_file():
            raise EvidenceError(f"missing restart READY evidence: {raw.relative_to(bundle)}")
        restart_ready = load_json(raw)
        if not isinstance(restart_ready, dict) or restart_ready.get("service") != "network_service":
            raise EvidenceError(f"{raw}: invalid READY")
        restart_to_ready.append(total)

    resources = load_csv(bundle / "resource_samples.csv")
    network_name = manifest["network_service_process_name"]
    network_rows = [row for row in resources if row.get("process") == network_name]
    minimum_resource = int(contract["minimumSamples"]["networkServiceResource"])
    if len(network_rows) < minimum_resource:
        raise EvidenceError(
            f"resource_samples.csv: need at least {minimum_resource} NetworkService rows"
        )
    rss_values: list[int] = []
    hwm_values: list[int] = []
    thread_values: list[int] = []
    fd_values: list[int] = []
    for row in network_rows:
        rss_values.append(as_int(row.get("rss_kb", ""), "resource:rss_kb"))
        hwm_values.append(as_int(row.get("hwm_kb", ""), "resource:hwm_kb"))
        thread_values.append(as_int(row.get("threads", ""), "resource:threads", minimum=1))
        fd_values.append(as_int(row.get("fd_count", ""), "resource:fd_count", minimum=1))

    allowed_operator = set(contract["allowedOperatorResults"])
    for key in contract["operatorAssertions"]:
        if operator[key] not in allowed_operator:
            raise EvidenceError(f"operator.env:{key} has invalid result {operator[key]!r}")

    smart_name = manifest.get("smartcontrol_process_name", "")
    smart_rows = [row for row in resources if smart_name and row.get("process") == smart_name]

    return {
        "schemaVersion": 1,
        "status": "EVIDENCE_COMPLETE_THRESHOLDS_UNFROZEN",
        "networkServiceRevision": provenance["network_service_revision"],
        "smartcontrolRevision": provenance["smartcontrol_revision"],
        "targetArch": manifest["target_arch"],
        "readyLatencyMs": ready_latency,
        "snapshotLatencyMs": snapshot_latency,
        "scanSamples": len(scans),
        "maxScanStartLatencyMs": max(scan_start_latencies),
        "maxScanCompletionMs": max(scan_completion),
        "restartSamples": len(restarts),
        "maxRestartToReadyMs": max(restart_to_ready),
        "networkServiceResourceSamples": len(network_rows),
        "maxNetworkServiceRssKb": max(max(rss_values), max(hwm_values)),
        "maxNetworkServiceThreads": max(thread_values),
        "maxNetworkServiceFd": max(fd_values),
        "smartcontrolResourceSamples": len(smart_rows),
        "operator": {
            key: operator[key] for key in contract["operatorAssertions"]
        },
    }


def apply_thresholds(result: dict[str, Any], thresholds: dict[str, int]) -> list[str]:
    checks = {
        "readyLatencyMs": "maxReadyLatencyMs",
        "maxScanStartLatencyMs": "maxScanStartLatencyMs",
        "maxScanCompletionMs": "maxScanCompletionMs",
        "maxRestartToReadyMs": "maxRestartToReadyMs",
        "maxNetworkServiceRssKb": "maxNetworkServiceRssKb",
        "maxNetworkServiceThreads": "maxNetworkServiceThreads",
        "maxNetworkServiceFd": "maxNetworkServiceFd",
    }
    failures: list[str] = []
    for observed_key, threshold_key in checks.items():
        if int(result[observed_key]) > thresholds[threshold_key]:
            failures.append(
                f"{observed_key}={result[observed_key]} exceeds "
                f"{threshold_key}={thresholds[threshold_key]}"
            )
    for key, value in result["operator"].items():
        if value != "pass":
            failures.append(f"operator assertion {key}={value!r}, expected 'pass'")
    return failures


def emit(result: dict[str, Any], path: Path | None) -> None:
    text = json.dumps(result, indent=2, sort_keys=True)
    print(text)
    if path is not None:
        path.write_text(text + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("bundle", type=Path)
    parser.add_argument("--contract", type=Path, default=DEFAULT_CONTRACT)
    parser.add_argument("--thresholds", type=Path)
    parser.add_argument("--structure-only", action="store_true")
    parser.add_argument("--json-out", type=Path)
    args = parser.parse_args()

    try:
        if not args.bundle.is_dir():
            raise EvidenceError(f"bundle is not a directory: {args.bundle}")
        result = validate_bundle(args.bundle, args.contract)
        if args.structure_only:
            emit(result, args.json_out)
            return 0
        if args.thresholds is None:
            result["status"] = "EVIDENCE_COMPLETE_THRESHOLDS_UNFROZEN"
            emit(result, args.json_out)
            return 3
        thresholds = load_thresholds(args.thresholds)
        failures = apply_thresholds(result, thresholds)
        result["thresholds"] = thresholds
        if failures:
            result["status"] = "RC_FAILED"
            result["failures"] = failures
            emit(result, args.json_out)
            return 4
        result["status"] = "RC_PROVEN"
        emit(result, args.json_out)
        return 0
    except EvidenceError as exc:
        result = {"schemaVersion": 1, "status": "EVIDENCE_INVALID", "error": str(exc)}
        emit(result, args.json_out)
        return 2


if __name__ == "__main__":
    sys.exit(main())
