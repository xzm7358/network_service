# NetworkService IPC v0 → EEP Network IPC v1 Migration Plan

## Status

Step 2.1 first wire-contract tranche is complete on the migration branch: v1 framing, HELLO/READY, request correlation, explicit v0/v1 coexistence, and the full first-tranche v1 contract CI gate are implemented and green. Event/reconnect/backpressure semantics remain follow-up work.

## Source

`NETWORKSERVICE-PRODUCT-IPC-SOURCE-V0` at product baseline commit `0b415f57c9bf2519645099513f7b73c18a174e8b`.

## Target

EEP Network IPC v1 as frozen by `docs/contracts/NETWORK_IPC_CONTRACT_V1.md`.

## Breaking dimensions

| Dimension | v0 | Target v1 |
|---|---|---|
| Framing | newline-delimited JSON | fixed 12-byte `NSP1` header + UTF-8 JSON; header ends with 4-byte big-endian payload length |
| Version negotiation | none | HELLO/version negotiation |
| Readiness | process/socket existence only | semantic READY session |
| Correlation | none | non-zero uint64 requestId |
| Event delivery | polling/snapshot style | sequenced/generation-aware events |
| Reconnect | client-specific behavior | authoritative snapshot rebase then newer events |
| Backpressure | implicit | bounded, observable overload semantics |

## Migration phases

### Phase A — Freeze and test v0

1. Keep `network-ipc-source-v0` immutable.
2. Keep contract-surface regression in product CI.
3. Record current consumers and commands before protocol removal.

### Phase B — Introduce protocol codec boundary

Completed in NS-IPC-103.

Binary framing/parse/encode responsibilities are isolated behind the v1 codec boundary while the v0 path remains intact.

### Phase C — Add v1 transport/session alongside v0

NS-IPC-104 through NS-IPC-106 establish the bounded dual-stack migration window:

1. v1 uses the frozen 12-byte `NSP1` header.
2. HELLO/version negotiation establishes semantic READY.
3. REQUEST/RESPONSE uses non-zero uint64 `requestId` correlation.
4. v0 and v1 share the same Unix-domain socket through an explicit connection-prefix selector.
5. v0 remains compatibility-only; no new consumer may adopt v0.

NS-IPC-110 promotes `tests/ipc_v1_contract_test.py` to a product CI release gate in both strict-host and ASan/UBSan jobs. The required first-tranche wire evidence therefore travels with every pull request and push to `main`.

#### Frozen protocol-selection boundary

Protocol selection is connection-scoped:

- first four octets exactly `NSP1` -> commit connection to v1;
- otherwise -> commit connection to frozen v0 newline-delimited JSON;
- after commitment there is no fallback in either direction;
- a malformed frame on an established v1 connection is rejected as v1 and cannot fall through to v0;
- `NSP1` appearing later inside a v0 JSON payload has no selection meaning.

This is a transport-prefix discriminator, not JSON field sniffing.

### Phase D — Event/reconnect semantics

1. Add subscription event sequence/generation.
2. On reconnect or gap, obtain authoritative snapshot.
3. Rebase consumer projection.
4. Consume only newer events.
5. Revisit the current single-client/serial accept loop and short accepted-client idle timeout before promoting long-lived EVENT sessions.

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
| existing SmartControl client | yes | not yet migrated | inventory/freeze exact client revision, then migrate explicitly to v1 |
| `tools/networkctl.py` | yes | no | intentionally remain v0 during the bounded migration window as a compatibility sentinel |
| future clients | forbidden | yes | v1 only |

## `tools/networkctl.py` migration decision

NS-IPC-106 freezes the following decision:

- keep the existing `tools/networkctl.py` wire behavior unchanged as v0 during the coexistence window;
- use it in coexistence regression tests to prove the frozen brownfield path remains usable;
- do not add silent v1 negotiation or automatic v1->v0 fallback;
- when a v1 debug client is needed, add an explicit v1 mode/client implementation and migrate deliberately;
- remove v0 behavior only under Phase E removal criteria.

This makes `networkctl.py` an intentional compatibility sentinel rather than an accidental legacy dependency.

## Release rule

Do not mark Network IPC v1 as adopted merely because new producer tests pass. Compatibility with existing consumers and real target restart/reconnect behavior is required evidence.
