# FlightWall v1.6.23

First full GitHub API OTA target.

- Keeps the current display page visible during routine background update checks.
- Removes the deliberate 25 ms black-frame presentation after the animated radar screen; only the backing framebuffer is cleared.
- Shows a dedicated DOWNLOADING UPDATE screen only after a newer signed firmware version has actually been found and installation begins.
- Keeps the GitHub-API-only updater, ASCII-hex detached signature transport, ECDSA/secp256k1 manifest verification, signed Git blob SHA binding, SHA-256 firmware verification, A/B staging, and rollback protection.
- RSA remains disabled so ADS-B stays on the proven ECDSA-only TLS configuration.
