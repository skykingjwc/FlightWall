# FlightWall v1.6.30

Raw-feed diagnostic build.

- Keeps direct raw.githubusercontent.com feed transport from v1.6.29.
- Reports the lwIP HTTP client result code and HTTP status when the feed request fails.
- This distinguishes DNS, connect/TLS, timeout, server-response, and other transport failures.
- No updater transport behavior changed beyond diagnostics.
