#!/usr/bin/env python3
import hashlib
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LOCK = ROOT / ".eep/adoption-lock.json"
DIAG = "PRODUCT_ADOPTION_METADATA_INVALID"


def sha256(path: Path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def verify(root=ROOT):
    errors = []
    required = [
        ".eep/project.json", ".eep/adopted-assets.json", ".eep/deviations.json",
        ".eep/adoption-lock.json", "docs/contracts/network-ipc-source-v0.json",
        "docs/migrations/NETWORK_IPC_V1_MIGRATION_PLAN.md"
    ]
    for rel in required:
        if not (root / rel).exists():
            errors.append(f"missing {rel}")
    if errors:
        return errors
    project = json.loads((root / ".eep/project.json").read_text())
    adopted = json.loads((root / ".eep/adopted-assets.json").read_text())
    deviations = json.loads((root / ".eep/deviations.json").read_text())
    lock = json.loads((root / ".eep/adoption-lock.json").read_text())
    source = json.loads((root / "docs/contracts/network-ipc-source-v0.json").read_text())
    commit = project.get("baselineCommit", "")
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        errors.append("baselineCommit must be immutable 40-hex SHA")
    if project.get("baselineRepository") != "xzm7358/network_service":
        errors.append("unexpected baseline repository")
    if project.get("platformRelease") != adopted.get("platformRelease"):
        errors.append("platform release mismatch")
    if source.get("implementationBaseline", {}).get("commit") != commit:
        errors.append("source-contract baseline commit mismatch")
    if not any(d.get("id") == "NET-BROWNFIELD-001" for d in deviations.get("deviations", [])):
        errors.append("IPC compatibility deviation missing")
    if lock.get("baselineCommit") != commit or lock.get("platformRelease") != project.get("platformRelease"):
        errors.append("adoption lock identity mismatch")
    for rel, expected in lock.get("documents", {}).items():
        path = root / rel
        if not path.exists() or sha256(path) != expected:
            errors.append(f"adoption lock hash mismatch: {rel}")
    return errors


def main():
    errors = verify()
    if errors:
        for error in errors:
            print(f"{DIAG}: {error}", file=sys.stderr)
        return 1
    print("EEP brownfield adoption metadata: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
