# Route DNS Policy

Default policy is Ethernet Preferred.

Rules:

- eth0 route metric: 10
- wlan0 route metric: 20
- Ethernet may write DNS.
- Wi-Fi may write DNS only when eth0 has no default route.
- Wi-Fi must not restart eth0 DHCP.
- Wi-Fi must not delete eth0 routes.
- Read-only snapshot and event APIs must not change route or DNS.

Future policy IPC can add ethernet_preferred, wifi_preferred, and wifi_only modes. This phase keeps ethernet_preferred as the fixed default.
