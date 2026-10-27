# FlightWall v1.5.0

This release completes the four-panel 128x64 interface.

- Keeps the five-second labeled startup quadrant grid.
- Rotates content inside each 64x32 tile while preserving the verified quadrant mapping.
- Expands the aircraft list from three to seven aircraft per page.
- Shows one tiny header dot per page, highlights the current page, and hides the indicator when only one page exists.
- Adds a web-configurable normal-screen duration from 3 to 30 seconds; the default is 6 seconds.
- Uses the full display for nearest-aircraft, radar, emergency, Wi-Fi, and OTA status screens.
- Adds compact LED artwork for DAL, UAL, AAL, UPS, FDX, SKW, RPA, and FFT callsigns.
- Falls back cleanly to the normal aircraft information layout for other callsigns.
- Enlarges the radar to nearly 60x60 pixels and animates a rotating sweep with a fading trail.
- Preserves Wi-Fi settings, watched aircraft, emergency detection and testing, full-panel load testing, and signed A/B OTA updates.

Existing v1-v3 saved settings migrate automatically. The new screen-duration
setting defaults to six seconds after migration.

The airline marks are hand-rendered, low-resolution LED adaptations implemented
as drawing code; the firmware does not bundle or download third-party image files.
