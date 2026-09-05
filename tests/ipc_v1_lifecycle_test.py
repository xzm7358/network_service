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
MAX_ACTIVE_CLIENTS = 8


def wait_socket(path: Path, process: subprocess.Popen, timeout: float = 3.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"server exited early: rc={process.returncode}")
        if path.exists():
            return
        time.sleep(0.02)
    raise RuntimeError("server socket did not appear")


def connect(path: Path, timeout: float = 1.0) -> socket.socket:
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
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
    magic, version, msg_type, flags, payload_len = HEADER.unpack(
        recv_exact(sock, HEADER.size)
    )
    if magic != MAGIC or version != VERSION or flags != 0:
        raise AssertionError(
            f"invalid v1 header magic={magic!r} version={version} flags={flags}"
        )
    if payload_len > MAX_PAYLOAD:
        raise AssertionError(f"oversized response payload: {payload_len}")
    payload = json.loads(recv_exact(sock, payload_len).decode("utf-8"))
    return msg_type, payload


def hello(sock: socket.socket, client_name: str):
    sock.sendall(
        encode_frame(
            TYPE_HELLO,
            {
                "minVersion": 1,
                "maxVersion": 1,
                "client": client_name,
                "capabilities": [],
            },
        )
    )
    msg_type, payload = recv_frame(sock)
    if msg_type != TYPE_READY:
        raise AssertionError(f"expected READY, got {msg_type} {payload}")
    if not payload.get("sessionId") or not isinstance(payload.get("generation"), int):
        raise AssertionError(f"invalid READY: {payload}")
    return payload


def request(sock: socket.socket, request_id: int, method: str):
    sock.sendall(
        encode_frame(
            TYPE_REQUEST,
            {
                "requestId": request_id,
                "method": method,
                "params": {},
            },
        )
    )
    msg_type, payload = recv_frame(sock)
    if msg_type != TYPE_RESPONSE:
        raise AssertionError(f"expected RESPONSE, got {msg_type} {payload}")
    if payload.get("requestId") != request_id or payload.get("status") != 200:
        raise AssertionError(f"response mismatch: {payload}")
    return payload


def test_simultaneous_sessions_and_long_idle(path: Path):
    first = connect(path, timeout=0.75)
    second = None
    try:
        first_ready = hello(first, "lifecycle-first")

        start = time.monotonic()
        second = connect(path, timeout=0.75)
        second_ready = hello(second, "lifecycle-second")
        elapsed = time.monotonic() - start
        if elapsed >= 0.75:
            raise AssertionError(
                f"second READY was blocked behind first session: {elapsed:.3f}s"
            )
        if first_ready["sessionId"] == second_ready["sessionId"]:
            raise AssertionError("simultaneous sessions reused sessionId")
        if first_ready["generation"] != second_ready["generation"]:
            raise AssertionError("same server process changed generation")

        # READY sessions must outlive the old inherited one-second read-idle bound.
        time.sleep(1.25)
        request(first, 101, "network.ping")
        request(second, 102, "network.ping")

        snapshot = request(second, 103, "network.snapshot")["result"]
        if snapshot.get("generation") != second_ready["generation"]:
            raise AssertionError(
                f"snapshot generation mismatch: {snapshot} {second_ready}"
            )
    finally:
        first.close()
        if second is not None:
            second.close()

    print("PASS test_simultaneous_sessions_and_long_idle")


def assert_capacity_rejected(path: Path):
    overflow = connect(path, timeout=0.75)
    try:
        try:
            overflow.sendall(
                encode_frame(
                    TYPE_HELLO,
                    {
                        "minVersion": 1,
                        "maxVersion": 1,
                        "client": "lifecycle-overflow",
                        "capabilities": [],
                    },
                )
            )
            data = overflow.recv(1)
            if data:
                raise AssertionError(
                    "connection beyond active-client bound received protocol data"
                )
        except (BrokenPipeError, ConnectionResetError, EOFError):
            return
        except socket.timeout as exc:
            raise AssertionError(
                "connection beyond active-client bound was not rejected promptly"
            ) from exc
    finally:
        overflow.close()


def test_capacity_and_recovery(path: Path):
    clients = []
    try:
        for index in range(MAX_ACTIVE_CLIENTS):
            sock = connect(path, timeout=1.0)
            hello(sock, f"capacity-{index}")
            clients.append(sock)

        assert_capacity_rejected(path)

        clients[0].close()
        clients.pop(0)
        time.sleep(0.05)

        replacement = connect(path, timeout=1.0)
        hello(replacement, "capacity-replacement")
        request(replacement, 201, "network.snapshot")
        clients.append(replacement)
    finally:
        for sock in clients:
            sock.close()

    print("PASS test_capacity_and_recovery")


def open_ready_clients(path: Path, count: int):
    clients = []
    try:
        for index in range(count):
            sock = connect(path, timeout=1.0)
            hello(sock, f"teardown-{index}")
            clients.append(sock)
        return clients
    except Exception:
        for sock in clients:
            sock.close()
        raise


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True)
    args = parser.parse_args()

    binary = Path(args.binary).resolve()
    if not binary.is_file():
        raise SystemExit(f"binary not found: {binary}")

    with tempfile.TemporaryDirectory(prefix="network-service-ipc-v1-lifecycle-") as td:
        root = Path(td)
        sock_path = root / "network.sock"
        config_dir = root / "config"
        event_dir = root / "no-wpa"
        config_dir.mkdir()

        process = subprocess.Popen(
            [
                str(binary),
                "--socket",
                str(sock_path),
                "--eth",
                "lo",
                "--wifi",
                "testwifi0",
                "--config-dir",
                str(config_dir),
                "--event-dir",
                str(event_dir),
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )

        teardown_clients = []
        try:
            wait_socket(sock_path, process)
            test_simultaneous_sessions_and_long_idle(sock_path)
            test_capacity_and_recovery(sock_path)

            teardown_clients = open_ready_clients(sock_path, 2)
            started = time.monotonic()
            process.terminate()
            process.wait(timeout=2.0)
            elapsed = time.monotonic() - started
            if elapsed >= 2.0:
                raise AssertionError(f"server teardown exceeded bound: {elapsed:.3f}s")
            print("PASS test_shutdown_closes_active_sessions")
        finally:
            for sock in teardown_clients:
                sock.close()
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=3.0)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
            stdout, stderr = process.communicate()

        if process.returncode not in (0, -15):
            raise AssertionError(
                f"unexpected server exit rc={process.returncode}\n"
                f"stdout={stdout}\nstderr={stderr}"
            )
        if "IPC_CLIENT_CAPACITY_REJECTED" not in stderr:
            raise AssertionError(
                "capacity test did not observe explicit rejection diagnostic\n"
                f"stdout={stdout}\nstderr={stderr}"
            )

    print("IPC v1 lifecycle reactor: PASS (3/3)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
