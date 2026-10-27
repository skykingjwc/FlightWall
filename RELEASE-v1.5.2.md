# FlightWall v1.5.2

- Redesigns the nearest-aircraft screen as a balanced two-column layout that
  uses both lower panels while keeping the callsign and airline mark together.
  Distance now includes a compact relative-bearing arrow.
- Removes the aircraft-list page indicator dots; multi-page lists continue to
  rotate normally.
- Restores radar aircraft returns to a single pixel.
- Draws the animated radar sweep below the blue rings and crosshairs so the
  sweep cannot cast a shadow over the grid.
- Brightens the radar range text.
- Labels transponder emergencies as `HIJACK`, `RADIO`, or `EMERGENCY`.
- Shows a dedicated stale-data screen and suppresses alerts from stale aircraft
  positions while continuing to retry the live feed.
- Preserves the validated v1.5.1 four-panel mapping, startup grid, settings,
  Wi-Fi provisioning, emergency test, load test, and signed A/B OTA behavior.
