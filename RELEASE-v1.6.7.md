# FlightWall v1.6.7

Diagnostic maintenance build to isolate the ADS-B HTTPS regression.

- Restores the exact pre-v1.6.4 mbedTLS feature set used when ADS-B was known to work.
- Removes RSA, PKCS#1 v1.5, and ECDHE-RSA support for this diagnostic build.
- Keeps the v1.6.5 30-second minimum spacing between ADS-B API attempts.
- Keeps the v1.6.6 startup GitHub update check disabled.

Expected side effect: GitHub update checks may fail in this build because raw.githubusercontent.com currently requires RSA-capable TLS from FlightWall.

Test result interpretation:
- If ADS-B works again, the RSA TLS feature change is the regression source.
- If ADS-B still fails, the cause lies elsewhere and needs deeper instrumentation.
