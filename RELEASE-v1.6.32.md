# FlightWall v1.6.32

Fix lwIP threading in the custom GitHub feed client.

- Keeps the v1.6.31 custom api.github.com feed request and headers.
- Fixes the cached-DNS path so altcp/TLS PCB creation and connect happen while the CYW43 lwIP lock is still held.
- Adds cyw43_arch_lwip_check() to the DNS completion path to catch invalid raw-lwIP context usage.
- No signing, feed, firmware verification, or updater-policy changes.
