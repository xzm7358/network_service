#!/usr/bin/env python3
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DIAG = "MAC_IDENTITY_POLICY_VIOLATION"
SOURCE_ROOTS = (ROOT / "src", ROOT / "include", ROOT / "packaging")
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".h", ".hpp", ".sh"}

PATTERNS = (
    ("SIOCSIFHWADDR ioctl", re.compile(r"\bSIOCSIFHWADDR\b")),
    (
        "ip link MAC mutation",
        re.compile(r"\bip\s+link\s+set\b[^\n;]*\baddress\b", re.IGNORECASE),
    ),
    (
        "ifconfig MAC mutation",
        re.compile(r"\bifconfig\b[^\n;]*(?:\bhw\s+ether\b|\bether\s+[0-9a-f]{2}:)", re.IGNORECASE),
    ),
    (
        "MAC setter helper",
        re.compile(
            r"\b(?:set|change|replace|randomize|generate)[A-Za-z0-9_]*(?:Mac|MAC|mac)(?:Address|Addr|Identity)?\s*\(",
        ),
    ),
)


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    text = re.sub(r"//[^\n]*", "", text)
    return text


def iter_sources() -> list[Path]:
    files: list[Path] = []
    for root in SOURCE_ROOTS:
        if not root.exists():
            continue
        for path in root.rglob("*"):
            if path.is_file() and path.suffix.lower() in SOURCE_SUFFIXES:
                files.append(path)
    return sorted(files)


def main() -> int:
    violations: list[str] = []
    for path in iter_sources():
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError as exc:
            violations.append(f"cannot read {path.relative_to(ROOT)}: {exc}")
            continue
        stripped = strip_comments(text)
        for label, pattern in PATTERNS:
            for match in pattern.finditer(stripped):
                line = stripped.count("\n", 0, match.start()) + 1
                excerpt = " ".join(match.group(0).split())[:160]
                violations.append(
                    f"{path.relative_to(ROOT)}:{line}: {label}: {excerpt!r}"
                )

    if violations:
        for violation in violations:
            print(f"{DIAG}: {violation}", file=sys.stderr)
        return 1

    print("NetworkService immutable Ethernet MAC policy: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
