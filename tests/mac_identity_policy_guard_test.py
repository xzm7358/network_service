#!/usr/bin/env python3
from __future__ import annotations

import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GUARD = ROOT / "tools" / "verify_mac_identity_policy.py"


def run(root: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["python3", str(GUARD), "--root", str(root)],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )


def write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="mac-policy-guard-") as tmp:
        root = Path(tmp)
        write(
            root / "src" / "safe.cpp",
            "const char *read_mac() { return \"/sys/class/net/eth0/address\"; }\n"
            "// SIOCSIFHWADDR is mentioned only in a comment and must not trigger.\n",
        )
        result = run(root)
        require(result.returncode == 0, f"read-only fixture rejected: {result.stderr}")

        cases = {
            "ioctl.cpp": "int x = SIOCSIFHWADDR;\n",
            "ip.sh": "ip link set dev eth0 address 02:11:22:33:44:55\n",
            "ifconfig.sh": "ifconfig eth0 hw ether 02:11:22:33:44:55\n",
            "setter.cpp": "void setMacAddress(const char *value) { (void)value; }\n",
        }
        for name, source in cases.items():
            case_root = root / name.replace(".", "_")
            write(case_root / ("packaging" if name.endswith(".sh") else "src") / name, source)
            result = run(case_root)
            require(result.returncode == 1, f"mutation fixture {name} was not rejected")
            require(
                "MAC_IDENTITY_POLICY_VIOLATION" in result.stderr,
                f"mutation fixture {name} missing diagnostic: {result.stderr}",
            )

    print("immutable MAC source guard regression: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
