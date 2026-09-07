#!/usr/bin/env python3
from __future__ import annotations

import json
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VALIDATOR = ROOT / "tools" / "rc" / "validate_ssd20x_evidence.py"
V1_CONTRACT = ROOT / "docs" / "contracts" / "ssd20x-rc-evidence-v1.json"


def write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def replace_env(path: Path, key: str, value: str) -> None:
    lines = path.read_text(encoding="utf-8").splitlines()
    prefix = key + "="
    replaced = False
    out: list[str] = []
    for line in lines:
        if line.startswith(prefix):
            out.append(prefix + value)
            replaced = True
        else:
            out.append(line)
    if not replaced:
        raise RuntimeError(f"missing env key {key} in {path}")
    path.write_text("\n".join(out) + "\n", encoding="utf-8")


def make_bundle(root: Path) -> Path:
    bundle = root / "bundle"
    write(
        bundle / "manifest.env",
        "\n".join(
            [
                "schema_version=2",
                "collector_version=2",
                "captured_at_utc=2026-09-07T00:00:00Z",
                "target_arch=armv7l",
                "target_uname=Linux fixture 4.9 armv7l",
                "network_service_socket=/tmp/smart_hmi_network.sock",
                "network_service_eth_iface=eth0",
                "network_service_binary=/dnake/bin/network_service",
                "network_service_binary_sha256=" + "a" * 64,
                "network_service_process_name=network_service",
                "network_service_pid=123",
                "smartcontrol_process_name=desktop",
                "smartcontrol_pid=456",
                "rc_probe_binary=/dnake/bin/network_service_rc_probe",
                "rc_probe_sha256=" + "b" * 64,
            ]
        )
        + "\n",
    )
    write(
        bundle / "provenance.env",
        "\n".join(
            [
                "network_service_revision=" + "1" * 40,
                "smartcontrol_revision=" + "2" * 40,
                "compiler_id=arm-linux-gnueabihf-g++ 8.2",
                "toolchain_id=ssd20x-sdk-2026.09",
                "sysroot_id=ssd20x-rootfs-v4",
                "build_id=rc-fixture-001",
            ]
        )
        + "\n",
    )
    write(
        bundle / "operator.env",
        "operator=fixture\n"
        "ui_responsive_during_scan=UNRECORDED\n"
        "ui_recovers_after_restart=UNRECORDED\n"
        "notes=host fixture\n",
    )
    write(bundle / "hashes.sha256", f"{'a' * 64}  /dnake/bin/network_service\n")
    write(bundle / "ready.metrics", "ready_latency_ms=24\n")
    write(bundle / "snapshot.metrics", "snapshot_latency_ms=11\n")
    write(bundle / "post_restart_snapshot.metrics", "snapshot_latency_ms=13\n")

    ready = {
        "version": 1,
        "service": "network_service",
        "sessionId": "fixture",
        "generation": 7,
        "capabilities": ["request-response", "events", "snapshot-rebase", "network-control"],
    }
    snapshot = {
        "requestId": 1,
        "status": 200,
        "result": {"generation": 7, "snapshotSeq": 3, "snapshot": {}},
    }
    write(bundle / "raw" / "ready.json", json.dumps(ready))
    write(bundle / "raw" / "snapshot.json", json.dumps(snapshot))
    write(bundle / "raw" / "post_restart_snapshot.json", json.dumps(snapshot))

    scan_rows = ["sample,scan_id,start_latency_ms,completion_ms,final_state"]
    for sample in range(1, 4):
        scan_rows.append(f"{sample},{sample},18,{800 + sample},ready")
        write(
            bundle / "raw" / f"scan_{sample}.start.json",
            json.dumps(
                {
                    "requestId": sample,
                    "status": 202,
                    "result": {"scanId": sample, "state": "scanning", "error": "", "results": []},
                }
            ),
        )
        write(
            bundle / "raw" / f"scan_{sample}.final.json",
            json.dumps(
                {
                    "requestId": sample + 10,
                    "status": 200,
                    "result": {"scanId": sample, "state": "ready", "error": "", "results": []},
                }
            ),
        )
    write(bundle / "scan_samples.csv", "\n".join(scan_rows) + "\n")

    restart_rows = ["sample,restart_to_ready_ms,probe_wait_ready_ms"]
    for sample in range(1, 3):
        restart_rows.append(f"{sample},{1100 + sample},90")
        restarted = dict(ready)
        restarted["generation"] = 7 + sample
        write(bundle / "raw" / f"restart_{sample}.ready.json", json.dumps(restarted))
    write(bundle / "restart_samples.csv", "\n".join(restart_rows) + "\n")

    resources = ["phase,monotonic_ms,process,pid,rss_kb,hwm_kb,threads,fd_count"]
    for sample in range(5):
        resources.append(
            f"steady_{sample + 1},{1000 + sample},network_service,123,{4200 + sample},5000,4,12"
        )
        resources.append(
            f"steady_{sample + 1},{1000 + sample},desktop,456,{20000 + sample},22000,9,30"
        )
    write(bundle / "resource_samples.csv", "\n".join(resources) + "\n")

    mac = "02:11:22:33:44:55"
    mac_rows = ["phase,monotonic_ms,iface,mac"]
    phases = ["baseline", "scan_1", "scan_2", "scan_3", "restart_1", "restart_2", "final"]
    for index, phase in enumerate(phases, 1):
        mac_rows.append(f"{phase},{2000 + index},eth0,{mac}")
    write(bundle / "mac_samples.csv", "\n".join(mac_rows) + "\n")
    return bundle


def run(bundle: Path, *args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["python3", str(VALIDATOR), str(bundle), *args],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="ssd20x-rc-evidence-") as tmp:
        root = Path(tmp)
        bundle = make_bundle(root)

        result = run(bundle, "--structure-only")
        require(result.returncode == 0, f"structure-only failed: {result.stdout} {result.stderr}")
        require("EVIDENCE_COMPLETE_THRESHOLDS_UNFROZEN" in result.stdout, "wrong structure status")
        require('"stable": true' in result.stdout, "stable MAC identity evidence missing")
        require('"baseline": "02:11:22:33:44:55"' in result.stdout, "MAC baseline missing")

        result = run(bundle)
        require(result.returncode == 3, "validator did not fail closed without thresholds")

        write(
            bundle / "operator.env",
            "operator=fixture\n"
            "ui_responsive_during_scan=pass\n"
            "ui_recovers_after_restart=pass\n"
            "notes=host fixture\n",
        )
        thresholds = root / "thresholds.json"
        thresholds.write_text(
            json.dumps(
                {
                    "schemaVersion": 1,
                    "maxReadyLatencyMs": 100,
                    "maxScanStartLatencyMs": 100,
                    "maxScanCompletionMs": 2000,
                    "maxRestartToReadyMs": 2000,
                    "maxNetworkServiceRssKb": 8000,
                    "maxNetworkServiceThreads": 8,
                    "maxNetworkServiceFd": 20,
                }
            ),
            encoding="utf-8",
        )
        result = run(bundle, "--thresholds", str(thresholds))
        require(result.returncode == 0, f"valid evidence did not prove RC: {result.stdout}")
        require('"status": "RC_PROVEN"' in result.stdout, "RC_PROVEN missing")

        changed_mac = root / "changed-mac"
        shutil.copytree(bundle, changed_mac)
        mac_text = (changed_mac / "mac_samples.csv").read_text(encoding="utf-8")
        mac_text = mac_text.replace(
            "restart_1,2005,eth0,02:11:22:33:44:55",
            "restart_1,2005,eth0,02:11:22:33:44:66",
        )
        (changed_mac / "mac_samples.csv").write_text(mac_text, encoding="utf-8")
        result = run(changed_mac, "--thresholds", str(thresholds))
        require(result.returncode == 4, "MAC identity change did not fail release proof")
        require('"status": "RC_FAILED"' in result.stdout, "MAC policy did not produce RC_FAILED")
        require("immutable Ethernet MAC policy violated" in result.stdout, "MAC policy diagnosis missing")

        strict_thresholds = root / "strict-thresholds.json"
        data = json.loads(thresholds.read_text(encoding="utf-8"))
        data["maxScanCompletionMs"] = 100
        strict_thresholds.write_text(json.dumps(data), encoding="utf-8")
        result = run(bundle, "--thresholds", str(strict_thresholds))
        require(result.returncode == 4, "threshold regression did not fail")
        require("maxScanCompletionMs" in result.stdout, "threshold failure not diagnosed")

        missing = root / "missing"
        shutil.copytree(bundle, missing)
        (missing / "raw" / "snapshot.json").unlink()
        result = run(missing, "--structure-only")
        require(result.returncode == 2, "missing required evidence did not fail")

        bad_sha = root / "bad-sha"
        shutil.copytree(bundle, bad_sha)
        provenance = (bad_sha / "provenance.env").read_text(encoding="utf-8")
        provenance = provenance.replace("1" * 40, "not-a-commit")
        (bad_sha / "provenance.env").write_text(provenance, encoding="utf-8")
        result = run(bad_sha, "--structure-only")
        require(result.returncode == 2, "invalid source revision did not fail")

        v1 = root / "v1"
        shutil.copytree(bundle, v1)
        replace_env(v1 / "manifest.env", "schema_version", "1")
        result = run(v1, "--contract", str(V1_CONTRACT), "--structure-only")
        require(result.returncode == 0, f"v1 compatibility regressed: {result.stdout}")
        require('"schemaVersion": 1' in result.stdout, "v1 schema result missing")

    print("ssd20x RC evidence validator regression: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
