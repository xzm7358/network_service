#!/usr/bin/env python3
import argparse
import os
import signal
import subprocess
import tempfile
import time


def wait_for_socket(path, proc, timeout=5.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            raise RuntimeError(f"server exited early rc={proc.returncode}")
        if os.path.exists(path):
            return
        time.sleep(0.05)
    raise RuntimeError("server socket did not appear")


def terminate(proc):
    if proc.poll() is not None:
        return
    proc.send_signal(signal.SIGTERM)
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=5)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True)
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="network-service-singleton-") as tempdir:
        socket1 = os.path.join(tempdir, "first.sock")
        socket2 = os.path.join(tempdir, "second.sock")
        common = ["--eth", "ns_eth_missing", "--wifi", "ns_wifi_missing",
                  "--config-dir", tempdir, "--event-dir", tempdir]

        first = subprocess.Popen(
            [args.binary, "--socket", socket1, *common],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        try:
            wait_for_socket(socket1, first)

            second = subprocess.run(
                [args.binary, "--socket", socket2, *common],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                timeout=5,
            )
            if second.returncode != 3:
                raise RuntimeError(
                    f"second daemon was not rejected rc={second.returncode} "
                    f"stdout={second.stdout!r} stderr={second.stderr!r}"
                )
            if "PROCESS_OWNERSHIP_REJECTED" not in second.stderr:
                raise RuntimeError("singleton rejection diagnostic missing")
            if first.poll() is not None:
                raise RuntimeError("singleton probe terminated the owning daemon")
        finally:
            terminate(first)

        third = subprocess.Popen(
            [args.binary, "--socket", socket2, *common],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        try:
            wait_for_socket(socket2, third)
        finally:
            terminate(third)

    print("network_service singleton regression: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
