#!/usr/bin/env python3
import argparse
import json
import socket
import struct
import subprocess
import tempfile
import time
from pathlib import Path

MAGIC = b"NSP1"
VERSION = 1
HEADER = struct.Struct(">4sBBHI")
MAX_PAYLOAD = 64 * 1024
TYPE_HELLO = 1
TYPE_READY = 2
TYPE_REQUEST = 3
TYPE_RESPONSE = 4


def encode_frame(msg_type: int, payload: dict) -> bytes:
    raw = json.dumps(payload, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
    if len(raw) > MAX_PAYLOAD:
        raise ValueError("payload too large")
    return HEADER.pack(MAGIC, VERSION, msg_type, 0, len(raw)) + raw


def recv_exact(sock: socket.socket, size: int) -> bytes:
    out = bytearray()
    while len(out) < size:
        chunk = sock.recv(size - len(out))
        if not chunk:
            raise EOFError("connection closed")
        out.extend(chunk)
    return bytes(out)


def recv_frame(sock: socket.socket):
    header = recv_exact(sock, HEADER.size)
    magic, version, msg_type, flags, payload_len = HEADER.unpack(header)
    assert magic == MAGIC
    assert version == VERSION
    assert flags == 0
    assert payload_len <= MAX_PAYLOAD
    payload = json.loads(recv_exact(sock, payload_len).decode("utf-8"))
    return msg_type, payload


def wait_socket(path: Path, process: subprocess.Popen, timeout: float = 3.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"server exited early: rc={process.returncode}")
        if path.exists():
            return
        time.sleep(0.02)
    raise RuntimeError("server socket did not appear")


def connect_ready(path: Path) -> socket.socket:
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.settimeout(1.5)
    sock.connect(str(path))
    sock.sendall(encode_frame(TYPE_HELLO, {
        "minVersion": 1,
        "maxVersion": 1,
        "client": "business-dispatch-test",
        "capabilities": [],
    }))
    msg_type, ready = recv_frame(sock)
    if msg_type != TYPE_READY:
        raise AssertionError(f"expected READY, got {msg_type} {ready}")
    if "network-control" not in ready.get("capabilities", []):
        raise AssertionError(f"READY missing network-control capability: {ready}")
    return sock


def request(sock: socket.socket, request_id: int, method: str, params=None):
    payload = {"requestId": request_id, "method": method}
    if params is not None:
        payload["params"] = params
    sock.sendall(encode_frame(TYPE_REQUEST, payload))
    msg_type, response = recv_frame(sock)
    if msg_type != TYPE_RESPONSE:
        raise AssertionError(f"expected RESPONSE for {method}, got {msg_type} {response}")
    if response.get("requestId") != request_id:
        raise AssertionError(f"requestId mismatch for {method}: {response}")
    return response


def test_read_dispatch_and_persistent_session(path: Path):
    with connect_ready(path) as sock:
        first = request(sock, 18446744073709551614, "eth.get_config", {})
        if first.get("status") != 200 or not isinstance(first.get("result"), dict):
            raise AssertionError(f"eth.get_config was not dispatched: {first}")

        second = request(sock, 22, "eth.get_config")
        if second.get("status") != 200:
            raise AssertionError(f"absent params must default to object: {second}")


def test_write_method_schema_rejected_without_session_teardown(path: Path):
    with connect_ready(path) as sock:
        bad = request(sock, 31, "wifi.connect", {"ssid": "Lab WiFi"})
        if bad.get("status") != 400:
            raise AssertionError(f"missing password must be 400: {bad}")
        error = bad.get("error") or {}
        if error.get("code") != "INVALID_PARAMS":
            raise AssertionError(f"wrong invalid-param error: {bad}")

        still_alive = request(sock, 32, "eth.get_config", {})
        if still_alive.get("status") != 200:
            raise AssertionError(f"application error tore down session: {still_alive}")


def test_boolean_schema_is_strict(path: Path):
    with connect_ready(path) as sock:
        bad = request(sock, 41, "wifi.set_enabled", {"enabled": "true"})
        if bad.get("status") != 400:
            raise AssertionError(f"string boolean must be rejected: {bad}")
        if (bad.get("error") or {}).get("code") != "INVALID_PARAMS":
            raise AssertionError(f"wrong boolean schema error: {bad}")


def test_scan_status_is_immediate_and_explicit(path: Path):
    with connect_ready(path) as sock:
        started = time.monotonic()
        response = request(sock, 45, "wifi.scan.status", {})
        elapsed = time.monotonic() - started
        if elapsed > 0.5:
            raise AssertionError(f"wifi.scan.status blocked for {elapsed:.3f}s")
        if response.get("status") != 200:
            raise AssertionError(f"wifi.scan.status failed: {response}")
        result = response.get("result") or {}
        if result.get("state") != "idle" or result.get("scanId") != 0:
            raise AssertionError(f"unexpected idle scan state: {response}")
        results = result.get("results") or {}
        if results.get("count") != 0 or results.get("aps") != []:
            raise AssertionError(f"idle scan results must be empty: {response}")


def test_unknown_method_correlated_404(path: Path):
    with connect_ready(path) as sock:
        response = request(sock, 51, "does.not.exist", {})
        if response.get("status") != 404:
            raise AssertionError(f"unknown method must be 404: {response}")
        if (response.get("error") or {}).get("code") != "METHOD_NOT_FOUND":
            raise AssertionError(f"unknown method error shape mismatch: {response}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True)
    args = parser.parse_args()
    binary = Path(args.binary).resolve()
    if not binary.is_file():
        raise SystemExit(f"binary not found: {binary}")

    tests = [
        test_read_dispatch_and_persistent_session,
        test_write_method_schema_rejected_without_session_teardown,
        test_boolean_schema_is_strict,
        test_scan_status_is_immediate_and_explicit,
        test_unknown_method_correlated_404,
    ]

    with tempfile.TemporaryDirectory(prefix="network-service-v1-business-") as td:
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
            for test in tests:
                test(sock_path)
                print(f"PASS {test.__name__}")
        finally:
            process.terminate()
            try:
                process.wait(timeout=2.0)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=2.0)
            if process.returncode not in (0, -15):
                stderr = process.stderr.read() if process.stderr else ""
                raise RuntimeError(f"server exited rc={process.returncode}: {stderr}")


if __name__ == "__main__":
    main()
