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
TYPE_RESPONSE = 4
MAX_PAYLOAD = 64 * 1024


def wait_socket(path: Path, process: subprocess.Popen, timeout: float = 3.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"server exited early: rc={process.returncode}")
        if path.exists():
            return
        time.sleep(0.02)
    raise RuntimeError("server socket did not appear")


def connect(path: Path, timeout: float = 2.0, receive_buffer=None) -> socket.socket:
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    if receive_buffer is not None:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, receive_buffer)
    sock.settimeout(timeout)
    sock.connect(str(path))
    return sock


def encode_frame(msg_type: int, payload: dict) -> bytes:
    raw = json.dumps(payload, separators=(",", ":")).encode("utf-8")
    if len(raw) > MAX_PAYLOAD:
        raise ValueError("payload too large")
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
    magic, version, msg_type, flags, payload_len = HEADER.unpack(recv_exact(sock, HEADER.size))
    if magic != MAGIC or version != VERSION or flags != 0:
        raise AssertionError(
            f"invalid v1 header magic={magic!r} version={version} flags={flags}"
        )
    if payload_len > MAX_PAYLOAD:
        raise AssertionError(f"oversized response payload: {payload_len}")
    payload = json.loads(recv_exact(sock, payload_len).decode("utf-8"))
    return msg_type, payload


def hello(sock: socket.socket):
    sock.sendall(encode_frame(TYPE_HELLO, {
        "minVersion": 1,
        "maxVersion": 1,
        "client": "backpressure-contract-test",
        "capabilities": [],
    }))
    msg_type, payload = recv_frame(sock)
    if msg_type != TYPE_READY:
        raise AssertionError(f"expected READY, got {msg_type} {payload}")
    if not payload.get("sessionId") or not isinstance(payload.get("generation"), int):
        raise AssertionError(f"invalid READY: {payload}")
    return payload


def snapshot(sock: socket.socket, request_id: int):
    sock.sendall(encode_frame(TYPE_REQUEST, {
        "requestId": request_id,
        "method": "network.snapshot",
        "params": {},
    }))
    msg_type, payload = recv_frame(sock)
    if msg_type != TYPE_RESPONSE:
        raise AssertionError(f"expected snapshot RESPONSE, got {msg_type} {payload}")
    if payload.get("requestId") != request_id or payload.get("status") != 200:
        raise AssertionError(f"snapshot response mismatch: {payload}")
    result = payload.get("result")
    if not isinstance(result, dict) or not isinstance(result.get("snapshot"), dict):
        raise AssertionError(f"snapshot rebase payload invalid: {payload}")
    return result


def generate_pressure(sock: socket.socket, duration: float = 1.2):
    frames = []
    for request_id in range(1, 20001):
        frames.append(encode_frame(TYPE_REQUEST, {
            "requestId": request_id,
            "method": "network.ping",
            "params": {},
        }))
    payload = b"".join(frames)
    view = memoryview(payload)
    offset = 0
    sock.setblocking(False)
    deadline = time.monotonic() + duration
    while offset < len(payload) and time.monotonic() < deadline:
        try:
            written = sock.send(view[offset:])
            if written == 0:
                break
            offset += written
        except (BlockingIOError, InterruptedError):
            time.sleep(0.001)
        except (BrokenPipeError, ConnectionResetError):
            break
    return offset


def test_slow_reader_is_evicted_and_recovery_rebases(path: Path):
    slow = connect(path, receive_buffer=1024)
    first_ready = hello(slow)
    sent = generate_pressure(slow)
    if sent < 64 * 1024:
        raise AssertionError(f"insufficient pressure bytes sent: {sent}")

    # Do not read any of the correlated responses. The server's bounded writer
    # must stop waiting for this client and return to accept within its deadline.
    time.sleep(1.4)

    with connect(path, timeout=2.5) as recovery:
        second_ready = hello(recovery)
        result = snapshot(recovery, 900001)
        if second_ready["sessionId"] == first_ready["sessionId"]:
            raise AssertionError("recovery reused overloaded sessionId")
        if result.get("generation") != second_ready["generation"]:
            raise AssertionError(f"snapshot/READY generation mismatch: {result} {second_ready}")
        if not isinstance(result.get("snapshotSeq"), int) or result["snapshotSeq"] < 0:
            raise AssertionError(f"invalid snapshot watermark: {result}")

    slow.close()
    print("PASS test_slow_reader_is_evicted_and_recovery_rebases")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True)
    args = parser.parse_args()

    binary = Path(args.binary).resolve()
    if not binary.is_file():
        raise SystemExit(f"binary not found: {binary}")

    with tempfile.TemporaryDirectory(prefix="network-service-ipc-v1-backpressure-") as td:
        root = Path(td)
        sock_path = root / "network.sock"
        config_dir = root / "config"
        event_dir = root / "no-wpa"
        config_dir.mkdir()

        process = subprocess.Popen(
            [str(binary),
             "--socket", str(sock_path),
             "--eth", "lo",
             "--wifi", "testwifi0",
             "--config-dir", str(config_dir),
             "--event-dir", str(event_dir)],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        try:
            wait_socket(sock_path, process)
            test_slow_reader_is_evicted_and_recovery_rebases(sock_path)
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=3.0)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
            stdout, stderr = process.communicate()

        if "IPC_V1_OUTBOUND_WRITE_STALLED" not in stderr:
            raise AssertionError(
                "slow-reader test recovered without observing the bounded write-stall diagnostic\n"
                f"stdout={stdout}\nstderr={stderr}"
            )

    print("IPC v1 bounded outbound backpressure: PASS (1/1)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
