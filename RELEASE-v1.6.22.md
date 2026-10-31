# FlightWall v1.6.22

GitHub API signature-fetch compatibility fix.

- Keeps the v1.6.21 GitHub-API-only updater architecture.
- Keeps the ADS-B-safe ECDSA-only mbedTLS configuration; RSA remains disabled.
- Stores the detached ECDSA manifest signature as ASCII hex text in `update/manifest.sig.hex`.
- Fetches that text through GitHub's Contents API and decodes it back into the original DER signature bytes on the Pico.
- Leaves manifest verification, SHA-256 firmware verification, Git blob SHA binding, A/B OTA staging, and rollback behavior unchanged.
