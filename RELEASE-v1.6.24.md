# FlightWall v1.6.24

Combined GitHub update-feed transport.

- Fetches the signed manifest and detached ECDSA signature in one GitHub Contents API request through `update/feed.json`.
- Eliminates the fragile second back-to-back GitHub API request that caused `SIGNATURE FETCH FAILED` on v1.6.22.
- The combined feed contains the exact manifest bytes as Base64 plus the exact detached signature as hex text.
- Signature verification remains ECDSA/secp256k1 against the embedded public key.
- Firmware SHA-256 verification, signed Git blob binding, A/B staging, and rollback protection are unchanged.
- Routine checks keep the current display page visible; a dedicated status screen appears only when a real firmware download begins.
