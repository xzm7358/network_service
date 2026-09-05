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
TYPE_HELLO = 1
TYPE_READY = 2
TYPE_REQUEST = 3
TYPE_ERROR = 6


def wait_socket(path: Path, process: subprocess.Popen, timeout: float = 3.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"server exited early: rc={process.returncode}")
        if path.exists():
            return
        time.sleep(0.02)
    raise RuntimeError("server socket did not appear")


def encode_frame(msg_type: int, payload: dict) -> bytes:
    raw = json.dumps(payload, separators=(",", ":")).encode("utf-8")
    return HEADER.pack(MAGIC, VERSION, msg_type, 0, len(raw)) + raw


def recv_exact(sock: socket.socket, size: int) -> bytes:
    out = bytearray()
    while len(out) < size:
        chunk = sock.recv(size - len(out))
        if not chunk:
            raise EOFError(f"connection closed with {size - len(out)} bytes remaining")
        out.extend(chunk)
    return bytes(out)


def recv_frame(sock: socket.socket):
    header = recv_exact(sock, HEADER.size)
    magic, version, msg_type, flags, payload_len = HEADER.unpack(header)
    if magic != MAGIC or version != VERSION or flags != 0:
        raise AssertionError(
            f"invalid response header: magic={magic!r} version={version} flags={flags}"
        )
    payload = json.loads(recv_exact(sock, payload_len).decode("utf-8"))
    return msg_type, payload


def connect(path: Path) -> socket.socket:
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.settimeout(2.0)
    sock.connect(str(path))
    return sock


def hello_frame(min_version=1, max_version=1):
    return encode_frame(TYPE_HELLO, {
        "minVersion": min_version,
        "maxVersion": max_version,
        "client": "session-test",
        "capabilities": [],
    })


def test_hello_ready(path: Path):
    with connect(path) as sock:
        sock.sendall(hello_frame())
        msg_type, payload = recv_frame(sock)
        if msg_type != TYPE_READY:
            raise AssertionError(f"expected READY, got type={msg_type} payload={payload}")
        if payload.get("version") != 1 or payload.get("service") != "network_service":
            raise AssertionError(f"invalid READY identity: {payload}")
        if not payload.get("sessionId"):
            raise AssertionError(f"READY missing sessionId: {payload}")
        if not isinstance(payload.get("generation"), int) or payload["generation"] <= 0:
            raise AssertionError(f"READY missing positive generation: {payload}")


def test_request_before_ready(path: Path):
    with connect(path) as sock:
        sock.sendall(encode_frame(TYPE_REQUEST, {
            "requestId": 1,
            "method": "network.ping",
            "params": {},
        }))
        msg_type, payload = recv_frame(sock)
        if msg_type != TYPE_ERROR or payload.get("code") != "SESSION_NOT_READY":
            raise AssertionError(f"pre-READY request not rejected correctly: {msg_type} {payload}")


def test_unsupported_version(path: Path):
    with connect(path) as sock:
        sock.sendall(hello_frame(2, 3))
        msg_type, payload = recv_frame(sock)
        if msg_type != TYPE_ERROR or payload.get("code") != "UNSUPPORTED_VERSION":
            raise AssertionError(f"unsupported version not rejected: {msg_type} {payload}")


def test_partial_hello(path: Path):
    frame = hello_frame()
    with connect(path) as sock:
        sock.sendall(frame[:3])
        time.sleep(0.01)
        sock.sendall(frame[3:HEADER.size])
        time.sleep(0.01)
        sock.sendall(frame[HEADER.size:])
        msg_type, payload = recv_frame(sock)
        if msg_type != TYPE_READY or payload.get("version") != 1:
            raise AssertionError(f"partial HELLO did not reach READY: {msg_type} {payload}")


def test_reconnect_gets_new_session(path: Path):
    ready_payloads = []
    for _ in range(2):
        with connect(path) as sock:
            sock.sendall(hello_frame())
            msg_type, payload = recv_frame(sock)
            if msg_type != TYPE_READY:
                raise AssertionError(f"expected READY during reconnect test: {msg_type} {payload}")
            ready_payloads.append(payload)

    first, second = ready_payloads
    if first.get("sessionId") == second.get("sessionId"):
        raise AssertionError(f"sessionId reused across reconnect: {first}")
    if first.get("generation") != second.get("generation"):
        raise AssertionError(f"generation changed without server restart: {ready_payloads}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True)
    args = parser.parse_args()
    binary = Path(args.binary).resolve()
    if not binary.is_file():
        raise SystemExit(f"binary not found: {binary}")

    tests = [
        test_hello_ready,
        test_request_before_ready,
        test_unsupported_version,
        test_partial_hello,
        test_reconnect_gets_new_session,
    ]

    with tempfile.TemporaryDirectory(prefix="network-service-ipc-v1-session-") as td:
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
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=3.0)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()

    print(f"IPC v1 session negotiation: PASS ({len(tests)}/{len(tests)})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
