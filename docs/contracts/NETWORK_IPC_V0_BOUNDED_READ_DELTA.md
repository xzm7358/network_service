# NetworkService IPC v0 Bounded-Read Safety Delta

The frozen `NETWORKSERVICE-PRODUCT-IPC-SOURCE-V0` document remains immutable evidence of commit
`0b415f57c9bf2519645099513f7b73c18a174e8b`.

This follow-up change adds one transport-safety delta without claiming EEP Network IPC v1 compatibility:

- an accepted client must provide progress within a 1000 ms idle window;
- an incomplete client that exceeds that idle window is closed;
- the server wake pipe participates in the per-client wait so SIGTERM/SIGINT can interrupt a stalled client;
- requests exceeding the observed 64 KiB limit are closed instead of being interpreted as a fallback snapshot request.

The wire shape for successful existing requests is unchanged. The delta exists to satisfy bounded callback/transport
behavior and failure containment while the larger v0 → EEP Network IPC v1 migration remains pending.
