#!/usr/bin/env python3
import argparse
import json
import os
import signal
import socket
import subprocess
import tempfile
import time
from pathlib import Path


def wait_socket(path: Path, process: subprocess.Popen, timeout: float = 3.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"server exited early: rc={process.returncode}")
        if path.exists():
            return
        time.sleep(0.02)
    raise RuntimeError("server socket did not appear")


def full_ping(path: Path, timeout: float):
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.settimeout(timeout)
    sock.connect(str(path))
    sock.sendall(b'{"method":"network.ping"}\n')
    sock.shutdown(socket.SHUT_WR)
    data = b""
    while True:
        chunk = sock.recv(4096)
        if not chunk:
            break
        data += chunk
    sock.close()
    obj = json.loads(data.decode())
    if obj.get("status") != 200:
        raise AssertionError(f"unexpected response: {obj}")
    return obj


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True)
    args = parser.parse_args()
    binary = Path(args.binary).resolve()
    if not binary.is_file():
        raise SystemExit(f"binary not found: {binary}")

    with tempfile.TemporaryDirectory(prefix="network-service-ipc-test-") as td:
        root = Path(td)
        sock_path = root / "network.sock"
        config_dir = root / "config"
        event_dir = root / "no-wpa"
        config_dir.mkdir()
        process = subprocess.Popen(
            [str(binary), "--socket", str(sock_path), "--eth", "lo", "--wifi", "testwifi0",
             "--config-dir", str(config_dir), "--event-dir", str(event_dir)],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        try:
            wait_socket(sock_path, process)

            stalled = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            stalled.connect(str(sock_path))
            stalled.sendall(b'{"method":"network.ping"')
            time.sleep(0.05)

            started = time.monotonic()
            full_ping(sock_path, timeout=2.5)
            elapsed = time.monotonic() - started
            if elapsed > 2.2:
                raise AssertionError(f"second client remained blocked too long: {elapsed:.3f}s")
            stalled.close()

            # Verify SIGTERM can wake a server that is currently waiting on an incomplete client.
            stalled2 = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            stalled2.connect(str(sock_path))
            stalled2.sendall(b'{"method":"network.ping"')
            time.sleep(0.05)
            process.send_signal(signal.SIGTERM)
            try:
                process.wait(timeout=3.0)
            except subprocess.TimeoutExpired as exc:
                process.kill()
                raise AssertionError("SIGTERM did not terminate a stalled IPC server within 3s") from exc
            stalled2.close()
            if process.returncode != 0:
                out, err = process.communicate()
                raise AssertionError(f"server shutdown rc={process.returncode}\nstdout={out}\nstderr={err}")
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=3.0)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()

    print("IPC stalled-client regression: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
