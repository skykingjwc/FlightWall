# FlightWall v1.6.26

Startup update-check delay.

- Delays the first automatic GitHub update check for 120 seconds after the initial ADS-B fetch.
- Manual Check for update requests from the web UI remain immediate.
- Keeps the callback-driven non-blocking updater introduced in v1.6.25.
- Routine update checks remain invisible on the display.
- Actual firmware downloads still show the dedicated DOWNLOADING UPDATE screen.
