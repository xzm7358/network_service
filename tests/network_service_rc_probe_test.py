#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import os
import signal
import subprocess
import tempfile
import time
from pathlib import Path


def wait_socket(path: Path, timeout: float = 3.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.exists():
            return
        time.sleep(0.02)
    raise RuntimeError(f"socket did not appear: {path}")


def start_fixture(binary: Path, socket: Path, state: Path, config: Path) -> subprocess.Popen[str]:
    proc = subprocess.Popen(
        [
            str(binary),
            "--socket",
            str(socket),
            "--state-file",
            str(state),
            "--config-dir",
            str(config),
            "--event-dir",
            str(config / "no-wpa"),
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    wait_socket(socket)
    return proc


def stop_fixture(proc: subprocess.Popen[str]) -> None:
    if proc.poll() is not None:
        stdout, stderr = proc.communicate(timeout=1)
        raise RuntimeError(f"fixture exited early rc={proc.returncode}: {stdout} {stderr}")
    proc.send_signal(signal.SIGTERM)
    try:
        stdout, stderr = proc.communicate(timeout=3)
    except subprocess.TimeoutExpired:
        proc.kill()
        stdout, stderr = proc.communicate(timeout=1)
        raise RuntimeError(f"fixture did not stop cleanly: {stdout} {stderr}")
    if proc.returncode != 0:
        raise RuntimeError(f"fixture stop rc={proc.returncode}: {stdout} {stderr}")


def run_probe(probe: Path, socket: Path, *args: str) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(
        [str(probe), "--socket", str(socket), *args],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        raise RuntimeError(f"probe failed: {result.stdout} {result.stderr}")
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--fixture", required=True, type=Path)
    parser.add_argument("--probe", required=True, type=Path)
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="network-service-rc-probe-") as tmp:
        root = Path(tmp)
        socket = root / "network.sock"
        state = root / "state.txt"
        config = root / "config"
        config.mkdir()
        state.write_text("disconnected\n", encoding="utf-8")

        fixture = start_fixture(args.fixture, socket, state, config)
        ready_path = root / "ready.json"
        result = run_probe(args.probe, socket, "--out", str(ready_path), "ready")
        if "ready_latency_ms=" not in result.stdout:
            raise RuntimeError("ready latency missing")
        ready = json.loads(ready_path.read_text(encoding="utf-8"))
        required = {"events", "snapshot-rebase", "network-control"}
        if ready.get("service") != "network_service" or not required.issubset(ready["capabilities"]):
            raise RuntimeError("probe did not validate real READY capabilities")

        snapshot_path = root / "snapshot.json"
        result = run_probe(args.probe, socket, "--out", str(snapshot_path), "snapshot")
        if "snapshot_latency_ms=" not in result.stdout:
            raise RuntimeError("snapshot latency missing")
        snapshot = json.loads(snapshot_path.read_text(encoding="utf-8"))
        if snapshot.get("status") != 200 or "snapshot" not in snapshot.get("result", {}):
            raise RuntimeError("probe snapshot evidence is invalid")

        clock = subprocess.run(
            [str(args.probe), "clock-ms"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            check=False,
        )
        if clock.returncode != 0 or int(clock.stdout.strip()) <= 0:
            raise RuntimeError("clock-ms failed")

        stop_fixture(fixture)
        if socket.exists():
            socket.unlink()

        wait_ready_path = root / "restart.ready.json"
        waiter = subprocess.Popen(
            [
                str(args.probe),
                "--socket",
                str(socket),
                "--poll-ms",
                "20",
                "--scan-timeout-ms",
                "3000",
                "--out",
                str(wait_ready_path),
                "wait-ready",
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        time.sleep(0.15)
        fixture = start_fixture(args.fixture, socket, state, config)
        stdout, stderr = waiter.communicate(timeout=4)
        if waiter.returncode != 0 or "ready_latency_ms=" not in stdout:
            raise RuntimeError(f"wait-ready failed: {stdout} {stderr}")
        restarted = json.loads(wait_ready_path.read_text(encoding="utf-8"))
        if restarted.get("generation") == ready.get("generation"):
            raise RuntimeError("real server restart did not change generation")
        stop_fixture(fixture)

    print("network_service RC probe real-server regression: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
