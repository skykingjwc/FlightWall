# FlightWall v1.6.36

OTA download safety fix.

- Moves OTA flash erase/program work out of the lwIP/TLS receive callback.
- Adds a 64 KiB decoded firmware queue between GitHub receive processing and flash programming.
- Applies TCP receive-window backpressure until the main loop drains queued firmware bytes.
- Keeps SHA-256 verification on the exact bytes written to the inactive OTA slot.
- Requires both received and flashed byte counts to match the signed manifest before accepting the update.
- Leaves the signed feed format, GitHub API transport, ADS-B async path, and A/B rollback behavior unchanged.
- The live update offer remains pointed at the older safe metadata until this build is manually validated.
