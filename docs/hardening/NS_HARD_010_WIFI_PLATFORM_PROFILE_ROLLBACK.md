# NS-HARD-010 — Wi-Fi Platform Profile Rollback

## Goal

Close the remaining Platform-level part of F-09. `ADD_NETWORK` creates an in-memory wpa_supplicant profile immediately. Until `wifi_create_profile()` successfully returns that id to Service, Platform owns the transient resource and must remove it if any later configuration step fails.

## Contract

After a successful `ADD_NETWORK`, each failure in SSID quoting, SSID configuration, open-network configuration, PSK quoting, or PSK configuration performs best-effort `REMOVE_NETWORK <id>` before returning failure.

Rollback diagnostics never replace the original failure reason. Persistence is not performed in this Platform rollback because the profile has not passed the Service activation/persistence boundary yet.

Together with NS-HARD-008, the complete create/connect transaction is:

1. Platform owns the id from `ADD_NETWORK` until `wifi_create_profile()` succeeds.
2. Service owns the returned transient profile until activation succeeds.
3. Only the successful Service path may persist the profile according to product policy.
