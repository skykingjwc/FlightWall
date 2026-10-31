# FlightWall v1.6.17

Automatic updater restoration experiment.

- Restores RSA support needed by raw.githubusercontent.com.
- Changes the GitHub updater to allocate its TLS context only during an active check/download.
- Frees the GitHub TLS context immediately after the operation so it does not remain resident beside the ADS-B TLS client.
- Queues the first signed GitHub update check only after the initial ADS-B fetch completes.
- Retains the signed manifest, ECDSA/secp256k1 verification, SHA-256 firmware verification, build-number downgrade protection, and A/B rollback behavior.
- Restores web-page wording for automatic signed update checks.
- Leaves the v1.6.16 display and manual-upload stabilization fixes intact.
