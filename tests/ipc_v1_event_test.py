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
TYPE_EVENT = 5
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


def connect(path: Path, timeout: float = 1.5) -> socket.socket:
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
        "client": "event-contract-test",
        "capabilities": [],
    }))
    msg_type, payload = recv_frame(sock)
    if msg_type != TYPE_READY:
        raise AssertionError(f"expected READY, got {msg_type} {payload}")
    capabilities = payload.get("capabilities")
    if not isinstance(capabilities, list) or "events" not in capabilities:
        raise AssertionError(f"READY does not advertise events capability: {payload}")
    if not isinstance(payload.get("generation"), int) or payload["generation"] <= 0:
        raise AssertionError(f"READY generation invalid: {payload}")
    if not payload.get("sessionId"):
        raise AssertionError(f"READY sessionId missing: {payload}")
    return payload


def subscribe(sock: socket.socket, request_id: int):
    sock.sendall(encode_frame(TYPE_REQUEST, {
        "requestId": request_id,
        "method": "network.events.subscribe",
        "params": {},
    }))

    response_type, response = recv_frame(sock)
    if response_type != TYPE_RESPONSE:
        raise AssertionError(f"subscribe expected RESPONSE, got {response_type} {response}")
    if response.get("requestId") != request_id or response.get("status") != 200:
        raise AssertionError(f"subscribe response correlation failed: {response}")
    if response.get("result") != {"subscribed": True}:
        raise AssertionError(f"subscribe result mismatch: {response}")

    event_type, event = recv_frame(sock)
    if event_type != TYPE_EVENT:
        raise AssertionError(f"subscribe expected EVENT, got {event_type} {event}")
    if event.get("event") != "network.events.subscribed":
        raise AssertionError(f"unexpected event name: {event}")
    if event.get("payload") != {}:
        raise AssertionError(f"subscription control payload mismatch: {event}")
    if not isinstance(event.get("generation"), int) or event["generation"] <= 0:
        raise AssertionError(f"EVENT generation invalid: {event}")
    if not isinstance(event.get("seq"), int) or event["seq"] <= 0:
        raise AssertionError(f"EVENT seq invalid: {event}")
    return event


def test_event_sequence_across_reconnect(path: Path):
    with connect(path) as first_sock:
        first_ready = hello(first_sock)
        first_event = subscribe(first_sock, 101)

    with connect(path) as second_sock:
        second_ready = hello(second_sock)
        second_event = subscribe(second_sock, 102)

    if first_ready["generation"] != first_event["generation"]:
        raise AssertionError(f"first READY/EVENT generation mismatch: {first_ready} {first_event}")
    if second_ready["generation"] != second_event["generation"]:
        raise AssertionError(f"second READY/EVENT generation mismatch: {second_ready} {second_event}")
    if first_ready["generation"] != second_ready["generation"]:
        raise AssertionError("server generation changed without server restart")
    if first_ready["sessionId"] == second_ready["sessionId"]:
        raise AssertionError("reconnect reused sessionId")
    if second_event["seq"] <= first_event["seq"]:
        raise AssertionError(
            f"EVENT sequence did not increase within generation: {first_event} -> {second_event}"
        )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True)
    args = parser.parse_args()

    binary = Path(args.binary).resolve()
    if not binary.is_file():
        raise SystemExit(f"binary not found: {binary}")

    with tempfile.TemporaryDirectory(prefix="network-service-ipc-v1-event-") as td:
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
            test_event_sequence_across_reconnect(sock_path)
            print("PASS test_event_sequence_across_reconnect")
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=3.0)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()

    print("IPC v1 EVENT sequencing/generation: PASS (1/1)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
