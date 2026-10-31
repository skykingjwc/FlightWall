# FlightWall v1.6.18

ADS-B recovery build after the v1.6.17 updater experiment.

- Removes RSA/ECDHE-RSA mbedTLS support again after confirming that merely compiling it in causes the ADS-B HTTPS regression (HTTP 0 ERR 4).
- Stops queuing the automatic GitHub update check at startup.
- Restores the proven ECDSA-only TLS feature set used by the working ADS-B builds.
- Keeps the v1.6.16 display, Delta-logo, radar-transition, and manual-browser-reconnect fixes.
- Updates the web page to clearly state that automatic GitHub updating is temporarily disabled while the transport is redesigned.
