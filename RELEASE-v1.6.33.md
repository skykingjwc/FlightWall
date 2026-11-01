# FlightWall v1.6.33

Compact signed GitHub API envelope.

- Removes the custom raw altcp feed client from v1.6.31/v1.6.32.
- Returns to lwIP's proven httpc_get_file_dns() HTTPS client.
- Fetches a compact update/check.json through api.github.com.
- The compact envelope carries the existing signed manifest fields plus the existing ECDSA signature.
- Reconstructs the exact canonical manifest bytes and verifies the existing signature before using any update metadata.
- Firmware blob downloads continue to use the existing GitHub Blob API path with SHA-256 verification.
- Keeps HTTPS serialization, manual-upload protection, and the two-minute automatic-check delay.
