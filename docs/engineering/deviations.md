# Active EEP Deviations

Machine-readable source: `.eep/deviations.json`.

## NET-BROWNFIELD-001 — Product IPC differs from Network IPC v1

Current product IPC is frozen as v0. It remains an explicit P1 migration item; the repository must not claim v1 compatibility before the governed migration completes.

## NET-BROWNFIELD-002 — Build output knows a SmartControl product path

Current CMake writes the binary under `app/smartcontrol/dnake/bin`. This adoption-only change does not alter packaging behavior. A dedicated follow-up should make NetworkService emit a repository-local/install artifact and move product placement to packaging/integration.
