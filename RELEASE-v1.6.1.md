# FlightWall v1.6.1

This is a signing-key recovery release and includes the radar sweep masking change.

- Rotates FlightWall to a new project-specific secp256k1 update-signing key.
- Documents the replacement public-key fingerprint.
- Requires one manual USB flash for devices already running v1.6.0 because the
  original v1.6.0 private key was not retained.
- Keeps RP2350 hardware secure boot disabled; no OTP/fuse provisioning is used.
- Keeps automatic updates authenticated by a signed manifest plus SHA-256 payload verification.
- Changes the radar sweep so animated green pixels are not drawn on fixed blue
  radar-grid pixels, avoiding the previous overwrite/underlay flicker.

After manually installing v1.6.1, later releases can again be delivered through
the signed automatic-update feed using the replacement private key retained by
the project owner.
