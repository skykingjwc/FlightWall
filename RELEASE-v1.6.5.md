# FlightWall v1.6.5

This maintenance release fixes ADS-B retry throttling after failed aircraft-data requests.

- Ensures every aircraft-data request attempt respects the configured refresh interval.
- Prevents rapid retry loops when no successful ADS-B response has been received yet.
- Keeps the GitHub RSA TLS compatibility fix from v1.6.4.
- Keeps the v1.6.3 radar sweep and web update controls unchanged.

With the default refresh interval of 30 seconds, FlightWall now waits at least
30 seconds between ADS-B API attempts whether the prior request succeeded or failed.
