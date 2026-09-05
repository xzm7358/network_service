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
TYPE_EVENT = 5
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


def encode_frame(msg_type: int, payload: dict, *, version: int = VERSION,
                 flags: int = 0, magic: bytes = MAGIC) -> bytes:
    raw = json.dumps(payload, separators=(",", ":")).encode("utf-8")
    if len(raw) > MAX_PAYLOAD:
        raise ValueError("payload too large")
    return HEADER.pack(magic, version, msg_type, flags, len(raw)) + raw


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
    if magic != MAGIC:
        raise AssertionError(f"bad response magic: {magic!r}")
    if version != VERSION:
        raise AssertionError(f"bad response version: {version}")
    if flags != 0:
        raise AssertionError(f"bad response flags: {flags}")
    if payload_len > MAX_PAYLOAD:
        raise AssertionError(f"oversized response payload: {payload_len}")
    payload = json.loads(recv_exact(sock, payload_len).decode("utf-8"))
    return msg_type, payload


def connect(path: Path, timeout: float = 1.5) -> socket.socket:
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.settimeout(timeout)
    sock.connect(str(path))
    return sock


def hello(sock: socket.socket):
    sock.sendall(encode_frame(TYPE_HELLO, {
        "minVersion": 1,
        "maxVersion": 1,
        "client": "contract-test",
        "capabilities": [],
    }))
    msg_type, payload = recv_frame(sock)
    if msg_type != TYPE_READY:
        raise AssertionError(f"HELLO expected READY, got type={msg_type} payload={payload}")
    if payload.get("version") != 1:
        raise AssertionError(f"READY version mismatch: {payload}")
    if payload.get("service") != "network_service":
        raise AssertionError(f"READY service mismatch: {payload}")
    if not payload.get("sessionId"):
        raise AssertionError(f"READY missing sessionId: {payload}")
    if not isinstance(payload.get("generation"), int):
        raise AssertionError(f"READY missing integer generation: {payload}")
    return payload


def assert_connection_rejected(sock: socket.socket, context: str):
    try:
        data = sock.recv(1)
    except (ConnectionResetError, BrokenPipeError, socket.timeout):
        return
    if data == b"":
        return
    raise AssertionError(f"{context}: expected rejection/close, received data={data!r}")


def test_valid_hello_ready(path: Path):
    with connect(path) as sock:
        hello(sock)


def test_request_before_hello_rejected(path: Path):
    with connect(path) as sock:
        sock.sendall(encode_frame(TYPE_REQUEST, {
            "requestId": 1,
            "method": "network.ping",
            "params": {},
        }))
        msg_type, payload = recv_frame(sock)
        if msg_type != TYPE_ERROR:
            raise AssertionError(f"pre-READY request expected ERROR, got {msg_type} {payload}")


def test_unsupported_version_rejected(path: Path):
    with connect(path) as sock:
        sock.sendall(encode_frame(TYPE_HELLO, {
            "minVersion": 2,
            "maxVersion": 2,
            "client": "contract-test",
            "capabilities": [],
        }))
        msg_type, payload = recv_frame(sock)
        if msg_type != TYPE_ERROR:
            raise AssertionError(f"unsupported version expected ERROR, got {msg_type} {payload}")


def test_request_id_correlation(path: Path):
    request_id = 9223372036854775807
    with connect(path) as sock:
        hello(sock)
        sock.sendall(encode_frame(TYPE_REQUEST, {
            "requestId": request_id,
            "method": "network.ping",
            "params": {},
        }))
        msg_type, payload = recv_frame(sock)
        if msg_type != TYPE_RESPONSE:
            raise AssertionError(f"REQUEST expected RESPONSE, got {msg_type} {payload}")
        if payload.get("requestId") != request_id:
            raise AssertionError(f"requestId not preserved exactly: {payload}")
        if payload.get("status") != 200:
            raise AssertionError(f"network.ping failed: {payload}")


def test_invalid_magic_rejected(path: Path):
    with connect(path) as sock:
        sock.sendall(encode_frame(TYPE_HELLO, {
            "minVersion": 1,
            "maxVersion": 1,
            "client": "contract-test",
            "capabilities": [],
        }, magic=b"BAD!"))
        assert_connection_rejected(sock, "invalid magic")


def test_invalid_header_version_rejected(path: Path):
    with connect(path) as sock:
        sock.sendall(encode_frame(TYPE_HELLO, {
            "minVersion": 1,
            "maxVersion": 1,
            "client": "contract-test",
            "capabilities": [],
        }, version=2))
        assert_connection_rejected(sock, "invalid header version")


def test_nonzero_flags_rejected(path: Path):
    with connect(path) as sock:
        sock.sendall(encode_frame(TYPE_HELLO, {
            "minVersion": 1,
            "maxVersion": 1,
            "client": "contract-test",
            "capabilities": [],
        }, flags=1))
        assert_connection_rejected(sock, "nonzero reserved flags")


def test_oversized_payload_rejected(path: Path):
    with connect(path) as sock:
        header = HEADER.pack(MAGIC, VERSION, TYPE_HELLO, 0, MAX_PAYLOAD + 1)
        sock.sendall(header)
        assert_connection_rejected(sock, "oversized payload")


def test_partial_frame_delivery(path: Path):
    frame = encode_frame(TYPE_HELLO, {
        "minVersion": 1,
        "maxVersion": 1,
        "client": "contract-test",
        "capabilities": [],
    })
    with connect(path) as sock:
        for cut in (3, HEADER.size, len(frame)):
            pass
        sock.sendall(frame[:3])
        time.sleep(0.01)
        sock.sendall(frame[3:HEADER.size])
        time.sleep(0.01)
        sock.sendall(frame[HEADER.size:])
        msg_type, payload = recv_frame(sock)
        if msg_type != TYPE_READY or payload.get("version") != 1:
            raise AssertionError(f"partial HELLO did not produce READY: {msg_type} {payload}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True)
    args = parser.parse_args()
    binary = Path(args.binary).resolve()
    if not binary.is_file():
        raise SystemExit(f"binary not found: {binary}")

    tests = [
        test_valid_hello_ready,
        test_request_before_hello_rejected,
        test_unsupported_version_rejected,
        test_request_id_correlation,
        test_invalid_magic_rejected,
        test_invalid_header_version_rejected,
        test_nonzero_flags_rejected,
        test_oversized_payload_rejected,
        test_partial_frame_delivery,
    ]

    with tempfile.TemporaryDirectory(prefix="network-service-ipc-v1-contract-") as td:
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

    print(f"IPC v1 contract: PASS ({len(tests)}/{len(tests)})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
