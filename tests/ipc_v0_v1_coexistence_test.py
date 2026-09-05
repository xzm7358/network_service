#!/usr/bin/env python3
import argparse
import json
import socket
import struct
import subprocess
import sys
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


def connect(path: Path, timeout: float = 2.0) -> socket.socket:
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.settimeout(timeout)
    sock.connect(str(path))
    return sock


def encode_v1(msg_type: int, payload: dict) -> bytes:
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


def recv_v1(sock: socket.socket):
    header = recv_exact(sock, HEADER.size)
    magic, version, msg_type, flags, payload_len = HEADER.unpack(header)
    if magic != MAGIC or version != VERSION or flags != 0:
        raise AssertionError(
            f"invalid v1 response header: magic={magic!r} version={version} flags={flags}"
        )
    if payload_len > MAX_PAYLOAD:
        raise AssertionError(f"oversized v1 response: {payload_len}")
    payload = json.loads(recv_exact(sock, payload_len).decode("utf-8"))
    return msg_type, payload


def recv_v0_line(sock: socket.socket) -> dict:
    data = bytearray()
    while b"\n" not in data:
        chunk = sock.recv(4096)
        if not chunk:
            break
        data.extend(chunk)
    if not data:
        raise AssertionError("v0 connection closed without response")
    if data.startswith(MAGIC):
        raise AssertionError(f"v0 request was misclassified as v1: {bytes(data[:16])!r}")
    line = bytes(data).split(b"\n", 1)[0]
    return json.loads(line.decode("utf-8"))


def hello(sock: socket.socket):
    sock.sendall(encode_v1(TYPE_HELLO, {
        "minVersion": 1,
        "maxVersion": 1,
        "client": "coexistence-test",
        "capabilities": [],
    }))
    msg_type, payload = recv_v1(sock)
    if msg_type != TYPE_READY:
        raise AssertionError(f"expected READY, got {msg_type} {payload}")
    return payload


def test_v0_ping(path: Path):
    with connect(path) as sock:
        sock.sendall(b'{"method":"network.ping"}\n')
        payload = recv_v0_line(sock)
    if payload.get("status") != 200:
        raise AssertionError(f"v0 ping failed: {payload}")


def test_v0_fragmented_with_leading_whitespace(path: Path):
    chunks = [b" ", b"\t", b'{"meth', b'od":"network.ping"}', b"\n"]
    with connect(path) as sock:
        for chunk in chunks:
            sock.sendall(chunk)
            time.sleep(0.005)
        payload = recv_v0_line(sock)
    if payload.get("status") != 200:
        raise AssertionError(f"fragmented v0 request failed: {payload}")


def test_v0_payload_may_contain_nsp1(path: Path):
    with connect(path) as sock:
        sock.sendall(b'{"method":"network.ping","note":"NSP1"}\n')
        payload = recv_v0_line(sock)
    if payload.get("status") != 200:
        raise AssertionError(f"v0 payload containing NSP1 was misclassified: {payload}")


def test_v1_ping(path: Path):
    request_id = 18446744073709551615
    with connect(path) as sock:
        hello(sock)
        sock.sendall(encode_v1(TYPE_REQUEST, {
            "requestId": request_id,
            "method": "network.ping",
            "params": {},
        }))
        msg_type, payload = recv_v1(sock)
    if msg_type != TYPE_RESPONSE:
        raise AssertionError(f"v1 ping expected RESPONSE, got {msg_type} {payload}")
    if payload.get("requestId") != request_id or payload.get("status") != 200:
        raise AssertionError(f"v1 ping correlation failed: {payload}")


def test_nsp1_prefix_never_falls_through_to_v0(path: Path):
    # Once the first four octets are NSP1, the connection is committed to v1.
    # This intentionally malformed v1 header must therefore be rejected, not
    # reinterpreted as newline-delimited v0 JSON.
    malformed = MAGIC + b'{"method":"network.ping"}\n'
    with connect(path, timeout=1.0) as sock:
        sock.sendall(malformed)
        try:
            data = sock.recv(4096)
        except (ConnectionResetError, BrokenPipeError):
            return
        except socket.timeout as exc:
            raise AssertionError("malformed NSP1 prefix was not deterministically rejected") from exc
    if data == b"":
        return
    if data.startswith(b"{") or b'"status":200' in data:
        raise AssertionError(f"NSP1 connection fell through to v0 parser: {data!r}")
    raise AssertionError(f"unexpected response to malformed NSP1 connection: {data!r}")


def test_networkctl_remains_v0(path: Path, networkctl: Path):
    completed = subprocess.run(
        [sys.executable, str(networkctl), "network.ping", "--socket", str(path)],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        timeout=3.0,
    )
    if completed.returncode != 0:
        raise AssertionError(
            f"networkctl v0 compatibility failed rc={completed.returncode}: {completed.stderr}"
        )
    payload = json.loads(completed.stdout.strip())
    if payload.get("status") != 200:
        raise AssertionError(f"networkctl v0 ping failed: {payload}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True)
    parser.add_argument("--networkctl", default="tools/networkctl.py")
    args = parser.parse_args()

    binary = Path(args.binary).resolve()
    networkctl = Path(args.networkctl).resolve()
    if not binary.is_file():
        raise SystemExit(f"binary not found: {binary}")
    if not networkctl.is_file():
        raise SystemExit(f"networkctl not found: {networkctl}")

    tests = [
        lambda path: test_v0_ping(path),
        lambda path: test_v0_fragmented_with_leading_whitespace(path),
        lambda path: test_v0_payload_may_contain_nsp1(path),
        lambda path: test_v1_ping(path),
        lambda path: test_nsp1_prefix_never_falls_through_to_v0(path),
        lambda path: test_networkctl_remains_v0(path, networkctl),
    ]
    names = [
        "test_v0_ping",
        "test_v0_fragmented_with_leading_whitespace",
        "test_v0_payload_may_contain_nsp1",
        "test_v1_ping",
        "test_nsp1_prefix_never_falls_through_to_v0",
        "test_networkctl_remains_v0",
    ]

    with tempfile.TemporaryDirectory(prefix="network-service-ipc-coexist-") as td:
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
            for name, test in zip(names, tests):
                test(sock_path)
                print(f"PASS {name}")
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=3.0)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()

    print(f"IPC v0/v1 coexistence: PASS ({len(tests)}/{len(tests)})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
