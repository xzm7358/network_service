#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def replace_once(rel: str, old: str, new: str) -> None:
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{rel}: expected one match, found {count}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8")


replace_once(
    "src/ipc/network_ipc_v1_business_dispatch.cpp",
    '''    } else if (method == "wifi.scan.status") {
        daemon_json = daemon.wifi_scan_status_json();
    } else if (method == "wifi.scan") {
        daemon_json = daemon.wifi_scan_json();
    } else if (method == "wifi.set_enabled") {
''',
    '''    } else if (method == "wifi.scan.status") {
        daemon_json = daemon.wifi_scan_status_json();
    } else if (method == "wifi.set_enabled") {
''',
)

replace_once(
    "tests/ipc_v1_business_dispatch_test.py",
    '''def test_unknown_method_correlated_404(path: Path):
''',
    '''def test_legacy_sync_scan_is_retired_from_v1(path: Path):
    with connect_ready(path) as sock:
        started = time.monotonic()
        response = request(sock, 46, "wifi.scan", {})
        elapsed = time.monotonic() - started
        if elapsed > 0.5:
            raise AssertionError(f"retired wifi.scan still blocked for {elapsed:.3f}s")
        if response.get("status") != 404:
            raise AssertionError(f"retired wifi.scan must be 404: {response}")
        if (response.get("error") or {}).get("code") != "METHOD_NOT_FOUND":
            raise AssertionError(f"retired wifi.scan error shape mismatch: {response}")


def test_unknown_method_correlated_404(path: Path):
''',
)

replace_once(
    "tests/ipc_v1_business_dispatch_test.py",
    '''        test_scan_status_is_immediate_and_explicit,
        test_unknown_method_correlated_404,
''',
    '''        test_scan_status_is_immediate_and_explicit,
        test_legacy_sync_scan_is_retired_from_v1,
        test_unknown_method_correlated_404,
''',
)

replace_once(
    "docs/contracts/WIFI_SCAN_LIFECYCLE_V1.md",
    '''Legacy `wifi.scan` remains temporarily available for brownfield compatibility and retains its synchronous behavior. New/updated SmartControl code MUST migrate to `wifi.scan.start` + `wifi.scan.status`. Removal of legacy `wifi.scan` requires consumer migration and joint RC evidence.
''',
    '''The synchronous `wifi.scan` method is retired from the IPC v1 business surface after SmartControl consumer migration. A v1 REQUEST for exact method `wifi.scan` MUST return correlated `404 METHOD_NOT_FOUND` and MUST NOT enter the physical scan path.

The frozen brownfield v0 compatibility path is intentionally different: v0 `wifi.scan` remains supported through the v0 newline-delimited JSON dispatcher until a separate v0-removal change satisfies the migration and real-target evidence requirements in `NETWORK_IPC_CONTRACT_V1.md`. Therefore the shared `NetworkDaemon::wifi_scan_json()` and synchronous backend helper remain only as v0 compatibility implementation and are not reachable from IPC v1.
''',
)
