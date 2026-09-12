#!/usr/bin/env python3
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DIAG = "PRODUCT_SERVICE_CONTRACT_OWNERSHIP_LEAK"
CPP_SUFFIXES = {".cpp", ".cc", ".c", ".h", ".hpp"}
INCLUDE_RX = re.compile(r'^\s*#\s*include\s*[<"]([^">]+)[">]')
CONTRACT_NAMES = (
    "EthernetConfig",
    "WifiApRecord",
    "WifiSavedNetwork",
    "DhcpLeaseFact",
)


def iter_cpp_files(base: Path):
    if not base.exists():
        return
    for path in base.rglob("*"):
        if path.suffix in CPP_SUFFIXES:
            yield path


def upward_include_findings(base: Path):
    findings = []
    for path in iter_cpp_files(base):
        for lineno, line in enumerate(path.read_text(errors="replace").splitlines(), 1):
            match = INCLUDE_RX.match(line)
            if not match:
                continue
            target = match.group(1).replace("\\", "/")
            parts = {part for part in target.split("/") if part not in {"", ".", ".."}}
            if parts.intersection({"platform", "config"}):
                findings.append((path, lineno, line.strip()))
    return findings


def cmake_service_platform_is_private(text: str):
    match = re.search(
        r"target_link_libraries\s*\(\s*network_service_service(?P<body>.*?)\n\)",
        text,
        re.S,
    )
    if not match:
        return False
    body = match.group("body")
    platform_pos = body.find("NetworkService::Platform")
    private_pos = body.rfind("PRIVATE", 0, platform_pos)
    public_pos = body.rfind("PUBLIC", 0, platform_pos)
    return platform_pos >= 0 and private_pos > public_pos


def scan(root=ROOT):
    errors = []

    # Service public contracts and every IPC translation unit/header must compile
    # without Platform/Config include visibility.
    for base in [root / "src/service/include/service", root / "src/ipc"]:
        for path, lineno, evidence in upward_include_findings(base):
            errors.append(f"{path.relative_to(root)}:{lineno}: {evidence}")

    contracts = root / "include/network_service_contracts.h"
    if not contracts.exists():
        errors.append("missing include/network_service_contracts.h")
    else:
        text = contracts.read_text(errors="replace")
        for name in CONTRACT_NAMES:
            if not re.search(rf"\bstruct\s+{re.escape(name)}\b", text):
                errors.append(f"neutral contract missing struct {name}")

    # DTO ownership must not drift back into mechanism/config headers.
    for base in [root / "src/platform/include", root / "src/config/include"]:
        for path in iter_cpp_files(base):
            text = path.read_text(errors="replace")
            for name in CONTRACT_NAMES:
                if re.search(rf"\bstruct\s+{re.escape(name)}\b", text):
                    errors.append(
                        f"{path.relative_to(root)}: redefines neutral contract {name}"
                    )

    cmake = root / "CMakeLists.txt"
    if not cmake.exists() or not cmake_service_platform_is_private(
        cmake.read_text(errors="replace") if cmake.exists() else ""
    ):
        errors.append(
            "NetworkService::Platform must remain a PRIVATE dependency of network_service_service"
        )

    return errors


def self_test():
    assert cmake_service_platform_is_private(
        "target_link_libraries(network_service_service\n"
        "    PUBLIC\n        network_service_build_config\n"
        "    PRIVATE\n        NetworkService::Platform\n)"
    )
    assert not cmake_service_platform_is_private(
        "target_link_libraries(network_service_service\n"
        "    PUBLIC\n        NetworkService::Platform\n)"
    )


def main():
    if "--self-test" in sys.argv:
        self_test()
        print("Service contract boundary verifier self-test: PASS")
        return 0

    errors = scan()
    if errors:
        for error in errors:
            print(f"{DIAG}: {error}", file=sys.stderr)
        return 1
    print("NetworkService Service contract ownership: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
