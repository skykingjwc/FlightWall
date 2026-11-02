# Signed update feed

FlightWall reads both update metadata and firmware directly from
`raw.githubusercontent.com` over HTTPS. The repository is public, so no
GitHub token is required on the device.

- `check.json` is the small signed update advertisement FlightWall checks.
- `manifest.json` is the canonical schema 3 document covered by the ECDSA signature.
- `manifest.sig` and `manifest.sig.hex` contain the same ECDSA/secp256k1 signature.
- `flightwall.uf2` is the raw inactive-slot OTA payload.
- `payload-info.json` records the staged payload metadata for release tooling.

Schema 3 intentionally keeps the signed identity small:

- `version`
- `build`
- `size`
- `sha256`

The Git blob SHA is no longer part of the device protocol. FlightWall streams
`update/flightwall.uf2` directly, verifies the exact byte count and SHA-256,
then asks the RP2350 ROM to boot it using try-before-you-buy protection.

## Release order

Publish the firmware payload first. Publish the matching signed `check.json`
last. That way a device can never discover a new release before its firmware
file exists. If the raw file and signed metadata ever disagree temporarily,
the size/SHA-256 check rejects the payload and leaves the current image active.

Create a signed release from a full Stage 3 UF2:

```sh
python3 tools/make_release.py \
  --version 1.6.42 \
  --build 10642 \
  --uf2 build/hub75_demo.uf2 \
  --private-key /secure/path/FlightWall-firmware-signing-private.pem
```

The script removes the leading partition-table block used only for USB
installation, pads the application stream to the OTA receiver's 2 KiB
boundary, calculates SHA-256, signs the canonical manifest, verifies the
signature, and writes all release metadata including `check.json`.

The private signing key must remain outside the repository.
