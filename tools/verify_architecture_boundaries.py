#!/usr/bin/env python3
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MECHANISMS = [
    r"\bwpa_cli\b",
    r"\budhcpc\b",
    r"\bifconfig\b",
    r"\broute\s+(?:add|del)\b",
    r"/etc/resolv\.conf",
    r"\bsystem\s*\(",
    r"\bpopen\s*\(",
]
DHCP_CALLBACK_FORBIDDEN = [
    r"\bifconfig\b",
    r"\broute\s+(?:add|del)\b",
    r"/etc/resolv\.conf",
]
PLATFORM_TRUTH_FORBIDDEN = [
    r"\.connected\s*=\s*[^;]*has_ip",
    r"\.online\s*=",
    r"\.network_ready\s*=",
]
DIAG = "PRODUCT_ARCHITECTURE_MECHANISM_LEAK"
DHCP_DIAG = "PRODUCT_DHCP_CALLBACK_POLICY_LEAK"
TRUTH_DIAG = "PRODUCT_PLATFORM_TRUTH_DERIVATION_LEAK"


def scan_text(path: Path, text: str, patterns=MECHANISMS):
    findings = []
    for pattern in patterns:
        rx = re.compile(pattern)
        for lineno, line in enumerate(text.splitlines(), 1):
            if rx.search(line):
                findings.append((path, lineno, pattern, line.strip()))
    return findings


def scan(root=ROOT):
    findings = []
    for base in [root / "src/service", root / "src/ipc"]:
        if not base.exists():
            continue
        for path in base.rglob("*"):
            if path.suffix in {".cpp", ".cc", ".c", ".h", ".hpp"}:
                for finding in scan_text(path, path.read_text(errors="replace")):
                    findings.append((DIAG, *finding))

    callback = root / "src/platform/udhcpc_process.cpp"
    if callback.exists():
        for finding in scan_text(
            callback,
            callback.read_text(errors="replace"),
            DHCP_CALLBACK_FORBIDDEN,
        ):
            findings.append((DHCP_DIAG, *finding))

    platform_snapshot = root / "src/platform/interface_snapshot.cpp"
    if platform_snapshot.exists():
        for finding in scan_text(
            platform_snapshot,
            platform_snapshot.read_text(errors="replace"),
            PLATFORM_TRUTH_FORBIDDEN,
        ):
            findings.append((TRUTH_DIAG, *finding))
    return findings


def self_test():
    assert scan_text(Path("bad.cpp"), 'system("wpa_cli -i wlan0 scan");')
    assert not scan_text(Path("good.cpp"), 'return wifi_scan(iface, error);')
    assert scan_text(
        Path("udhcpc_process.cpp"),
        'f << "route add default";',
        DHCP_CALLBACK_FORBIDDEN,
    )
    assert scan_text(
        Path("udhcpc_process.cpp"),
        'f << ": > /etc/resolv.conf";',
        DHCP_CALLBACK_FORBIDDEN,
    )
    assert not scan_text(
        Path("udhcpc_process.cpp"),
        'f << "printf lease fact";',
        DHCP_CALLBACK_FORBIDDEN,
    )
    assert scan_text(
        Path("interface_snapshot.cpp"),
        "snapshot.wifi.connected = snapshot.wifi.has_ip;",
        PLATFORM_TRUTH_FORBIDDEN,
    )
    assert scan_text(
        Path("interface_snapshot.cpp"),
        "snapshot.online = snapshot.dns_available;",
        PLATFORM_TRUTH_FORBIDDEN,
    )
    assert not scan_text(
        Path("interface_snapshot.cpp"),
        "snapshot.dns_available = !snapshot.dns4.empty();",
        PLATFORM_TRUTH_FORBIDDEN,
    )


def main():
    if "--self-test" in sys.argv:
        self_test()
        print("Architecture-boundary verifier self-test: PASS")
        return 0
    findings = scan()
    if findings:
        for diag, path, line, pattern, evidence in findings:
            try:
                rel = path.relative_to(ROOT)
            except ValueError:
                rel = path
            print(f"{diag}: {rel}:{line}: {evidence}", file=sys.stderr)
        return 1
    print("NetworkService architecture boundary: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
