#!/usr/bin/env python3
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CPP_SUFFIXES = {".cpp", ".cc", ".c", ".h", ".hpp"}
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
REPRESENTATION_FORBIDDEN = [
    r"\b[A-Za-z_][A-Za-z0-9_]*_json\s*\(",
    r"\b[A-Za-z_][A-Za-z0-9_]*_to_json\s*\(",
    r"\bpayload_json\s*\(",
]
WIFI_PLATFORM_POLICY_FORBIDDEN = [
    r"\bwifi_connect\s*\(",
    r"\bwifi_connect_saved\s*\(",
    r"\bwifi_forget_saved\s*\(",
    r"\bwifi_set_autoconnect\s*\(",
]
INCLUDE_RX = re.compile(r'^\s*#\s*include\s*[<"]([^">]+)[">]')
BROAD_SRC_CMAKE_RX = re.compile(
    r"\$<BUILD_INTERFACE:\$\{CMAKE_CURRENT_SOURCE_DIR\}/src>"
)
BROAD_SRC_CI_RX = re.compile(r"(^|\s)-Isrc(?=\s|[\"'])")

DIAG = "PRODUCT_ARCHITECTURE_MECHANISM_LEAK"
DHCP_DIAG = "PRODUCT_DHCP_CALLBACK_POLICY_LEAK"
TRUTH_DIAG = "PRODUCT_PLATFORM_TRUTH_DERIVATION_LEAK"
REPRESENTATION_DIAG = "PRODUCT_REPRESENTATION_BOUNDARY_LEAK"
WIFI_POLICY_DIAG = "PRODUCT_WIFI_PLATFORM_POLICY_LEAK"
INCLUDE_DIAG = "PRODUCT_INCLUDE_DEPENDENCY_DIRECTION_LEAK"
HEADER_LAYOUT_DIAG = "PRODUCT_LAYER_HEADER_LAYOUT_LEAK"
BROAD_INCLUDE_DIAG = "PRODUCT_BROAD_SRC_INCLUDE_ROOT_LEAK"


def scan_text(path: Path, text: str, patterns=MECHANISMS):
    findings = []
    for pattern in patterns:
        rx = re.compile(pattern)
        for lineno, line in enumerate(text.splitlines(), 1):
            if rx.search(line):
                findings.append((path, lineno, pattern, line.strip()))
    return findings


def scan_include_text(path: Path, text: str, forbidden_layers):
    findings = []
    for lineno, line in enumerate(text.splitlines(), 1):
        match = INCLUDE_RX.match(line)
        if not match:
            continue
        target = match.group(1).replace("\\", "/")
        parts = {part for part in target.split("/") if part not in {"", ".", ".."}}
        forbidden = sorted(parts.intersection(forbidden_layers))
        if forbidden:
            findings.append(
                (path, lineno, ",".join(forbidden), line.strip())
            )
    return findings


def iter_cpp_files(base: Path):
    if not base.exists():
        return
    for path in base.rglob("*"):
        if path.suffix in CPP_SUFFIXES:
            yield path


def scan(root=ROOT):
    findings = []
    for base in [root / "src/service", root / "src/ipc"]:
        for path in iter_cpp_files(base):
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

    for base in [root / "src/service", root / "src/platform", root / "src/config"]:
        for path in iter_cpp_files(base):
            for finding in scan_text(
                path,
                path.read_text(errors="replace"),
                REPRESENTATION_FORBIDDEN,
            ):
                findings.append((REPRESENTATION_DIAG, *finding))

    for path in [
        root / "src/platform/include/platform/wifi_backend.h",
        root / "src/platform/wifi_backend.cpp",
    ]:
        if not path.exists():
            continue
        for finding in scan_text(
            path,
            path.read_text(errors="replace"),
            WIFI_PLATFORM_POLICY_FORBIDDEN,
        ):
            findings.append((WIFI_POLICY_DIAG, *finding))

    # Include direction: lower layers must never reach upward, even through
    # relative-path includes that could bypass target include search paths.
    include_rules = [
        (root / "src/platform", {"service", "ipc"}),
        (root / "src/config", {"service", "ipc"}),
        (root / "src/service", {"ipc"}),
        (root / "include", {"platform", "config", "service", "ipc"}),
    ]
    for base, forbidden_layers in include_rules:
        for path in iter_cpp_files(base):
            for finding in scan_include_text(
                path,
                path.read_text(errors="replace"),
                forbidden_layers,
            ):
                findings.append((INCLUDE_DIAG, *finding))

    # Internal headers must live behind a namespaced layer include root. A new
    # header dropped directly beside implementation files would not participate
    # in the physical CMake boundary and is therefore rejected.
    for base in [
        root / "src/platform",
        root / "src/config",
        root / "src/service",
        root / "src/ipc",
    ]:
        if not base.exists():
            continue
        for suffix in ("*.h", "*.hpp"):
            for path in base.glob(suffix):
                findings.append(
                    (
                        HEADER_LAYOUT_DIAG,
                        path,
                        1,
                        "layer-header-layout",
                        "internal header must live under <layer>/include/<namespace>/",
                    )
                )

    cmake = root / "CMakeLists.txt"
    if cmake.exists():
        for finding in scan_text(
            cmake,
            cmake.read_text(errors="replace"),
            [BROAD_SRC_CMAKE_RX.pattern],
        ):
            findings.append((BROAD_INCLUDE_DIAG, *finding))

    workflow = root / ".github/workflows/eep-ci.yml"
    if workflow.exists():
        for finding in scan_text(
            workflow,
            workflow.read_text(errors="replace"),
            [BROAD_SRC_CI_RX.pattern],
        ):
            findings.append((BROAD_INCLUDE_DIAG, *finding))

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
    assert scan_text(
        Path("network_daemon.cpp"),
        "std::string snapshot_json() const;",
        REPRESENTATION_FORBIDDEN,
    )
    assert scan_text(
        Path("wifi_backend.cpp"),
        "std::string wifi_scan_to_json(const Records &records);",
        REPRESENTATION_FORBIDDEN,
    )
    assert scan_text(
        Path("network_state.cpp"),
        "return changes.payload_json();",
        REPRESENTATION_FORBIDDEN,
    )
    assert not scan_text(
        Path("network_daemon.cpp"),
        "NetworkSnapshot snapshot() const;",
        REPRESENTATION_FORBIDDEN,
    )
    assert scan_text(
        Path("wifi_backend.cpp"),
        "bool wifi_connect(const std::string &iface);",
        WIFI_PLATFORM_POLICY_FORBIDDEN,
    )
    assert scan_text(
        Path("wifi_backend.cpp"),
        "return wifi_set_autoconnect(iface, ssid, enabled, error);",
        WIFI_PLATFORM_POLICY_FORBIDDEN,
    )
    assert not scan_text(
        Path("wifi_backend.cpp"),
        'return wpa_ok(iface, "DISABLE_NETWORK all", error);',
        WIFI_PLATFORM_POLICY_FORBIDDEN,
    )
    assert not scan_text(
        Path("wifi_backend.cpp"),
        'return wpa_ok(iface, "SAVE_CONFIG", error);',
        WIFI_PLATFORM_POLICY_FORBIDDEN,
    )

    assert scan_include_text(
        Path("platform.cpp"),
        '#include "service/network_daemon.h"',
        {"service", "ipc"},
    )
    assert scan_include_text(
        Path("platform.cpp"),
        '#include "../service/include/service/network_daemon.h"',
        {"service", "ipc"},
    )
    assert not scan_include_text(
        Path("service.cpp"),
        '#include "platform/wifi_backend.h"',
        {"ipc"},
    )
    assert scan_include_text(
        Path("service.cpp"),
        '#include "ipc/network_ipc_server.h"',
        {"ipc"},
    )
    assert not BROAD_SRC_CMAKE_RX.search(
        '$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/platform/include>'
    )
    assert BROAD_SRC_CMAKE_RX.search(
        '$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>'
    )
    assert not BROAD_SRC_CI_RX.search('clang++ -Isrc/platform/include foo.cpp')
    assert BROAD_SRC_CI_RX.search('clang++ -Iinclude -Isrc "${f}"')


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
