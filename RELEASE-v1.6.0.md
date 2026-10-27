# FlightWall v1.6.0

- Adds signed automatic update checks at startup and every six hours.
- Pins a project-specific secp256k1 public key in firmware.
- Verifies the update manifest signature before trusting its contents.
- Verifies the complete OTA payload with SHA-256 before activating it.
- Retains the inactive A/B slot and RP2350 Try-Before-You-Buy rollback path.
- Adds automatic-update status to the embedded configuration page.
- Moves release signing to an external private key that is excluded from Git.
- Keeps manual browser uploads available for local recovery and migration.

Existing v1.5.2 devices require one manual installation of v1.6.0. Later
versions can be installed automatically from the signed GitHub feed.

For the initial installation, copy
[`release/FlightWall-v1.6.0.uf2`](release/FlightWall-v1.6.0.uf2) to the Pico 2 W
while it is in BOOTSEL mode. Do not use `update/flightwall.uf2` for USB or the
browser uploader; that file is the stripped payload used only by the automatic
updater.
