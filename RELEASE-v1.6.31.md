# FlightWall v1.6.31

Custom GitHub API feed transport.

- Uses api.github.com with the proven ECDSA-only TLS profile.
- Replaces lwIP's canned HTTP client for the update feed with a minimal altcp TLS client.
- Sends an explicit User-Agent and GitHub raw-content Accept header.
- Requests update/feed.json through the GitHub Contents API but asks GitHub to return the raw file body directly.
- Keeps the existing ECDSA manifest verification, signed blob binding, SHA-256 firmware verification, A/B staging, and rollback protection.
- Firmware blob downloads remain on the existing GitHub Blob API path.
- Keeps HTTPS serialization, manual-upload protection, and the two-minute automatic-check delay.
