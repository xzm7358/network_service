# NS-VAL-001 — Ubuntu Physical Integration Baseline

## Purpose

Use a dedicated Ubuntu physical machine as the pre-target validation layer before SSD20x/SSD201. This baseline validates real Linux process/socket/Netlink/wpa_supplicant behavior, idle resource stability, and ownership assumptions without pretending that x86 Ubuntu proves SSD20x memory, BusyBox, kernel-4.9, ARM ABI, or Wi-Fi-driver behavior.

## Required topology

Use three distinct interfaces:

```text
management NIC  -> SSH / host management only
                   (NetworkManager may own this interface)

test Ethernet   -> NetworkService validation only

test Wi-Fi      -> NetworkService + dedicated wpa_supplicant only
```

The invariant is **one interface = one network owner**. Do not run NetworkManager/systemd-networkd and NetworkService concurrently on either test interface.

A practical setup is a motherboard Ethernet NIC for management, one USB Ethernet adapter for the test Ethernet interface, and one USB Wi-Fi adapter for the test Wi-Fi interface.

## Safety properties

`ubuntu_preflight.sh` performs no network mutation. It fails closed when:

- management, test Ethernet, and test Wi-Fi are not explicitly provided and distinct;
- an interface does not exist;
- the NetworkService binary or required target-compatible commands are missing;
- the host default route is not proven to use the management NIC;
- the current SSH route, when observable through `SSH_CONNECTION`, is not proven to use the management NIC;
- a test interface already carries a default route;
- NetworkManager reports either test interface as managed;
- the dedicated wpa_supplicant ctrl socket is missing;
- the chosen validation UDS path is already occupied.

`networkctl`/systemd-networkd observations are warnings because distributions expose setup state differently; inspect and disable any competing owner before continuing.

## Host preparation

Build the product first:

```sh
cmake -S . -B build/ubuntu-val -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/ubuntu-val --parallel
```

Make the test interfaces unmanaged before validation. If NetworkManager is present, an immediate lab-only setup commonly looks like:

```sh
sudo nmcli device set <test-eth> managed no
sudo nmcli device set <test-wifi> managed no
```

Make this persistent using the host's normal NetworkManager/networkd configuration before long soak tests. Do not mark the management interface unmanaged.

Run a dedicated wpa_supplicant for the test Wi-Fi interface and ensure its control socket is available at:

```text
/var/run/wpa_supplicant/<test-wifi>
```

NS-VAL-001 does not auto-edit host network-manager configuration and does not auto-create Wi-Fi credentials.

## Preflight only

Example role assignment:

```sh
export NS_VAL_MGMT_IFACE=enp1s0
export NS_VAL_ETH_IFACE=enx001122334455
export NS_VAL_WIFI_IFACE=wlp3s0
export NS_VAL_BINARY="$PWD/app/smartcontrol/dnake/bin/network_service"

sudo -E sh tools/validation/ubuntu_preflight.sh
```

Do not continue until the final line is `RESULT|PREFLIGHT_PASS|...`.

## Run the baseline

Default collection time is 60 seconds at one-second intervals:

```sh
sudo -E sh tools/validation/ubuntu_baseline.sh
```

Useful overrides:

```sh
export NS_VAL_DURATION_SECONDS=300
export NS_VAL_INTERVAL_SECONDS=1
export NS_VAL_EVIDENCE_ROOT="$PWD/evidence/ubuntu"
export NS_VAL_SOCKET=/tmp/ns_val_network.sock
export NS_VAL_WPA_CTRL_DIR=/var/run/wpa_supplicant
```

The runner starts NetworkService directly with an isolated validation socket and config directory, waits for `network.ping`, allows WPA/Netlink initialization to settle, samples runtime state, then terminates the validation daemon unless `NS_VAL_KEEP_DAEMON=1` is explicitly set.

## Evidence package

Each run creates:

```text
evidence/ubuntu/<UTC>_<commit>/
├── manifest.env
├── preflight.log
├── network_service.log
├── network_service.pid
├── ping.json
├── resource.csv
├── snapshot.log
├── final-snapshot.json
├── process.log
├── processes.log
├── network-state.log
├── resolv.conf.log
├── runtime-config/
└── result.env
```

`resource.csv` records:

- VmRSS / VmSize / VmData / VmStk;
- `/proc/<pid>/fd` count;
- `/proc/<pid>/task` count and `Threads`;
- process user/system CPU ticks;
- udhcpc instance counts for each test interface;
- a time-correlated IPC snapshot log.

## NS-VAL-001 pass criteria

This Ubuntu baseline intentionally does **not** apply SSD20x RAM/CPU thresholds. It fails only on structural stability conditions that should hold on every platform:

```text
NetworkService survives the full sample window
FD end == FD start
thread/task end == thread/task start
udhcpc count for each test interface never exceeds 1
```

RSS start/end/peak/drift are evidence, not an Ubuntu release threshold. SSD20x resource budgets are enforced later on the target.

## What this baseline proves

A passing run establishes a physical-Linux baseline for:

- daemon/UDS startup and shutdown;
- WPA/Netlink initialization on real NICs;
- no immediate FD/thread drift;
- no duplicate DHCP process under idle observation;
- coherent evidence linking kernel network state, resolver state, process state, and IPC snapshot.

It does not yet inject faults. The next validation slice is NS-VAL-002, which will deliberately kill udhcpc/wpa_supplicant, delete owned IPv4/routes, restart NetworkService, and verify bounded recovery from the evidence stream.
