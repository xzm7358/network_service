# Ethernet MAC Identity Policy

Status: **Frozen for SSD20x RC**

## Decision

The Ethernet hardware/interface MAC address is immutable at runtime.

NetworkService is the authoritative owner of this policy. For this product release there is no automatic MAC remediation path: NetworkService does not generate, persist, or live-apply a replacement MAC after an IP/MAC conflict.

SmartControl may detect and surface a conflict for diagnostics/UI, but it must not mutate or persist interface identity.

## Rationale

The retired SmartControl behavior could execute a partial sequence equivalent to generating/persisting a new MAC and then applying it to the live interface. Any failure between persistence and live apply can split persistent identity from kernel identity. That is a high-cost recovery state on a wall panel and there is no current product requirement that requires runtime MAC rotation or conflict-driven MAC generation.

Freezing identity removes the transaction entirely:

- no replacement identity to persist;
- no live mutation that can partially fail;
- reboot/restart naturally retains the platform-provided identity;
- conflict handling cannot silently change device identity on the LAN.

## Runtime semantics

When a MAC/IP conflict is detected:

- diagnostic result: conflict present;
- remediation policy: `diagnostic-only`;
- live MAC mutation supported: `false`;
- replacement MAC persistence: `none`;
- automatic retry/remediation: `none`;
- remediation-failure state: not applicable because no mutation is attempted.

A change of the Ethernet MAC during NetworkService operation, Wi-Fi scanning, NetworkService restart, or RC HIL is a **policy violation**, not successful conflict remediation.

## Forbidden NetworkService implementation primitives

Production NetworkService source must not introduce runtime MAC mutation through mechanisms including:

- `SIOCSIFHWADDR`;
- `ip link set ... address ...`;
- `ifconfig ... hw ether ...`;
- equivalent helper functions whose purpose is setting/generating/randomizing interface MAC identity.

The CI guard `tools/verify_mac_identity_policy.py` rejects these mutation primitives in production source.

Read-only identity inspection (for example `SIOCGIFHWADDR` or `/sys/class/net/<iface>/address`) remains allowed.

## SmartControl boundary

SmartControl's legacy `ipwatchd` may observe ARP conflicts and report them through its diagnostic/UI path. Its network provider boundary remains responsible for preventing direct interface mutation. The NetworkService policy does not re-introduce a SmartControl network control path.

## SSD20x HIL evidence

SSD20x RC evidence schema v2 records the Ethernet MAC from `/sys/class/net/<iface>/address` at:

1. baseline;
2. after every physical Wi-Fi scan cycle;
3. after every NetworkService restart/recovery cycle;
4. final capture.

All recorded values must be valid canonical MAC addresses and must equal the baseline. The host validator treats any identity change as `RC_FAILED` independently of performance/resource thresholds.

## Change control

Supporting runtime MAC mutation in a future product is a new architecture decision. It requires a new contract/ADR defining atomic persistence + live apply, rollback/recovery, diagnostic semantics, target HIL, and an explicit migration away from this immutable policy. It must not be introduced as an incidental conflict-recovery patch.
