# FlightWall v1.6.4

This maintenance release fixes automatic-update HTTPS compatibility with GitHub.

- Adds ECDHE-RSA TLS support required by the current RSA certificate served by raw.githubusercontent.com.
- Retains ECDHE-ECDSA support.
- Keeps the v1.6.3 radar sweep and web update controls unchanged.
- Keeps signed-manifest verification, SHA-256 payload verification, and OTA rollback protection.

Because v1.6.1 through v1.6.3 cannot complete the HTTPS manifest fetch against
GitHub's current RSA certificate, install v1.6.4 once through the manual USB/BOOTSEL
path. After that, signed automatic updates can work normally again.
