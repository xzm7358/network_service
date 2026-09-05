# NetworkService IPC v0 → EEP Network IPC v1 Migration Plan

## Status

Planned; not yet implemented.

## Source

`NETWORKSERVICE-PRODUCT-IPC-SOURCE-V0` at product baseline commit `0b415f57c9bf2519645099513f7b73c18a174e8b`.

## Target

EEP Network IPC v1.

## Breaking dimensions

| Dimension | v0 | Target v1 |
|---|---|---|
| Framing | newline-delimited JSON | 4-byte big-endian length + UTF-8 JSON |
| Version negotiation | none | hello/version negotiation |
| Readiness | process/socket existence only | semantic READY session |
| Correlation | none | requestId |
| Event delivery | polling/snapshot style | sequenced/generation-aware events |
| Reconnect | client-specific behavior | authoritative snapshot rebase then newer events |
| Backpressure | implicit | bounded, observable overload semantics |

## Migration phases

### Phase A — Freeze and test v0

1. Keep `network-ipc-source-v0` immutable.
2. Add contract-surface regression to product CI.
3. Record current consumers and commands before protocol change.

### Phase B — Introduce protocol codec boundary

Move framing/parse/encode responsibilities behind a protocol codec so the IPC server no longer owns ad-hoc JSON extraction semantics.

No product behavior change is required in this phase.

### Phase C — Add v1 transport/session alongside v0 where feasible

1. Add length framing.
2. Add hello/version negotiation.
3. Add requestId.
4. Define READY.
5. Preserve explicit v0 compatibility path only for the bounded migration window if dual-stack cost is acceptable.

If dual-stack is not acceptable on the target, treat rollout as a coordinated breaking deployment and version the client/service package together.

### Phase D — Event/reconnect semantics

1. Add subscription event sequence/generation.
2. On reconnect or gap, obtain authoritative snapshot.
3. Rebase consumer projection.
4. Consume only newer events.

### Phase E — Remove v0

Removal requires:

- all known product consumers migrated;
- compatibility matrix updated;
- product CI passing contract/integration tests;
- real target restart/reconnect evidence;
- explicit migration record approval.

## Consumer compatibility matrix

| Consumer | v0 | v1 | Action |
|---|---:|---:|---|
| existing SmartControl client | yes | unknown | inventory/freeze exact client revision before migration |
| `tools/networkctl.py` | yes | no | migrate or provide compatibility adapter |
| future clients | no new adoption | yes | target v1 only |

## Release rule

Do not mark Network IPC v1 as adopted merely because new producer tests pass. Compatibility with existing consumers and real target restart/reconnect behavior is required evidence.
