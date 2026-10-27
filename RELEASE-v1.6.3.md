# FlightWall v1.6.3

This release improves the radar sweep and adds user-facing controls for signed firmware updates.

- Softens the radar sweep leading edge so it no longer dominates the scope.
- Extends the fading sweep tail from 12 degrees to 24 degrees.
- Keeps every sweep ray off the fixed blue radar rings and crosshairs.
- Adds a **Check for update** button to the embedded web interface.
- Adds an **Install available update** button for user-controlled installs.
- Adds a persistent **Automatically install signed updates** preference.
- Automatic install remains enabled by default for existing devices.
- When automatic install is disabled, FlightWall still checks the signed feed at
  startup and every six hours but reports an available version without downloading it.
- Keeps the manual browser UF2 uploader under an expandable recovery section.
- Retains signed-manifest verification, payload SHA-256 verification, and OTA rollback protection.

The automatic-update preference is stored with the other FlightWall settings and
v1.6.2/v4 settings migrate with automatic installation enabled.
