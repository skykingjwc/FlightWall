# FlightWall v1.6.21

GitHub API automatic-updater implementation.

- Keeps the ADS-B-safe ECDSA-only mbedTLS configuration; RSA remains disabled.
- Moves update transport entirely to api.github.com.
- Fetches manifest.json and manifest.sig through the public Repository Contents API.
- Introduces signed manifest schema 2 with a deterministic Git blob SHA.
- Downloads firmware through the Git Blob API using the signed blob SHA.
- Streams GitHub's base64 blob content directly into the inactive A/B slot without buffering the whole UF2 in RAM.
- Verifies the manifest ECDSA/secp256k1 signature and decoded firmware SHA-256 before installation.
- Public repository access requires no GitHub token.
- Restores the normal Check for update and Install available update controls.
