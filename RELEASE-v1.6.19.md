# FlightWall v1.6.19

GitHub API HTTPS compatibility diagnostic.

- Keeps the ADS-B-safe ECDSA-only mbedTLS configuration.
- Does not compile RSA/ECDHE-RSA support.
- Changes the updater host for this diagnostic to api.github.com.
- The update check buttons perform only a small HTTPS transport probe against the GitHub API.
- Any real HTTP response code is treated as proof that the TLS handshake succeeded.
- No firmware is downloaded or installed by the diagnostic.
- Automatic updating remains disabled until the API transport test is validated on hardware.
- Retains the v1.6.18 ADS-B recovery and all prior display/manual-upload fixes.
