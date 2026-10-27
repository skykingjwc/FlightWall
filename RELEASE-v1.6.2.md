# FlightWall v1.6.2

This release fixes the v1.6.1 recovery installer and completes the signing-key recovery.

- Separates the manual USB recovery image from the automatic OTA image.
- The USB recovery UF2 is built as a normal RP2350 image without the TBYB flag.
- The OTA image retains RP2350 Try-Before-You-Buy rollback protection.
- Keeps the replacement FlightWall secp256k1 update-signing key introduced during recovery.
- Includes the radar sweep masking change that skips fixed blue radar-grid pixels.
- Keeps hardware secure boot and OTP provisioning disabled.

Devices running v1.6.0 should install `release/FlightWall-v1.6.2.uf2` once
through BOOTSEL. After that, future releases can use the signed automatic-update feed.

v1.6.1 is superseded for manual USB recovery because its USB image was
accidentally built with the TBYB flag enabled and could fall back to the prior image.
