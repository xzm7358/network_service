#!/usr/bin/env python3
import argparse
import json
import socket
import sys

DEFAULT_SOCKET = "/tmp/smart_hmi_network.sock"


def request(socket_path: str, method: str) -> int:
    payload = json.dumps({"method": method}, separators=(",", ":")) + "\n"
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
        sock.connect(socket_path)
        sock.sendall(payload.encode("utf-8"))
        chunks = []
        while True:
            chunk = sock.recv(4096)
            if not chunk:
                break
            chunks.append(chunk)
            if b"\n" in chunk:
                break
    sys.stdout.write(b"".join(chunks).decode("utf-8", errors="replace"))
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="NetworkService debug client")
    parser.add_argument("method", nargs="?", default="network.snapshot")
    parser.add_argument("--socket", default=DEFAULT_SOCKET)
    args = parser.parse_args()
    return request(args.socket, args.method)


if __name__ == "__main__":
    raise SystemExit(main())
