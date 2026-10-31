# FlightWall v1.6.28

Serialize FlightWall's HTTPS clients.

- Prevents ADS-B refreshes from starting while the GitHub updater has an active HTTPS request.
- Keeps page rotation running from the last known aircraft data while an update check is in progress.
- Removes an overlapping TLS-client race between opendata.adsb.fi and api.github.com.
- Retains the two-minute automatic-check delay and manual-upload protection.
- Retains callback-driven GitHub update checks and firmware downloads.
