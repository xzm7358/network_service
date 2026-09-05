#!/usr/bin/env python3
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CONTRACT = ROOT / "docs/contracts/network-ipc-source-v0.json"
SERVER = ROOT / "src/ipc/network_ipc_server.cpp"
HEADER = ROOT / "include/network_service_protocol.h"

DIAG = "PRODUCT_IPC_SOURCE_CONTRACT_DRIFT"


def load_contract(path=CONTRACT):
    return json.loads(Path(path).read_text())


def discover(header_text: str, server_text: str):
    constants = dict(re.findall(r'constexpr\s+const\s+char\s*\*\s*(kMethod\w+)\s*=\s*"([^"]+)"', header_text))
    methods = set(re.findall(r'method\s*==\s*"([^"]+)"', server_text))
    for name in re.findall(r'method\s*==\s*(kMethod\w+)', server_text):
        if name in constants:
            methods.add(constants[name])
    declared = set(constants.values())
    return methods, declared


def verify(root=ROOT):
    contract = load_contract(root / "docs/contracts/network-ipc-source-v0.json")
    header = (root / "include/network_service_protocol.h").read_text()
    server = (root / "src/ipc/network_ipc_server.cpp").read_text()
    methods, declared = discover(header, server)
    expected = set(contract["implementedMethods"])
    declared_only = set(contract["declaredButNotImplementedMethods"])
    errors = []
    if methods != expected:
        errors.append(f"implemented methods drift: expected={sorted(expected)} actual={sorted(methods)}")
    actual_declared_only = declared - methods
    if actual_declared_only != declared_only:
        errors.append(f"declared-only methods drift: expected={sorted(declared_only)} actual={sorted(actual_declared_only)}")
    path = contract["transport"]["defaultPath"]
    if path not in header:
        errors.append(f"socket path drift: {path}")
    if "64 * 1024" not in server and "64*1024" not in server:
        errors.append("observed 64 KiB request threshold disappeared")
    if "request.find('\\n')" not in server and 'request.find("\\n")' not in server:
        errors.append("newline request framing disappeared")
    return errors


def self_test():
    h = 'constexpr const char *kMethodPing = "network.ping";\nconstexpr const char *kMethodSubscribe = "network.subscribe";'
    s = 'if (method == kMethodPing) {}\nif (method == "wifi.scan") {}'
    methods, declared = discover(h, s)
    assert methods == {"network.ping", "wifi.scan"}
    assert declared - methods == {"network.subscribe"}


def main():
    if "--self-test" in sys.argv:
        self_test()
        print("IPC source-contract verifier self-test: PASS")
        return 0
    errors = verify()
    if errors:
        for error in errors:
            print(f"{DIAG}: {error}", file=sys.stderr)
        return 1
    print("NetworkService IPC source contract v0: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
