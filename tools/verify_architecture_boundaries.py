#!/usr/bin/env python3
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MECHANISMS = [r"\bwpa_cli\b", r"\budhcpc\b", r"\bifconfig\b", r"\broute\s+(?:add|del)\b", r"/etc/resolv\.conf", r"\bsystem\s*\(", r"\bpopen\s*\("]
HIGH_LEVEL = [ROOT / "src/service", ROOT / "src/ipc"]
DIAG = "PRODUCT_ARCHITECTURE_MECHANISM_LEAK"


def scan_text(path: Path, text: str):
    findings = []
    for pattern in MECHANISMS:
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
                findings.extend(scan_text(path, path.read_text(errors="replace")))
    return findings


def self_test():
    assert scan_text(Path("bad.cpp"), 'system("wpa_cli -i wlan0 scan");')
    assert not scan_text(Path("good.cpp"), 'return wifi_scan(iface, error);')


def main():
    if "--self-test" in sys.argv:
        self_test()
        print("Architecture-boundary verifier self-test: PASS")
        return 0
    findings = scan()
    if findings:
        for path, line, pattern, evidence in findings:
            try:
                rel = path.relative_to(ROOT)
            except ValueError:
                rel = path
            print(f"{DIAG}: {rel}:{line}: {evidence}", file=sys.stderr)
        return 1
    print("NetworkService architecture boundary: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
