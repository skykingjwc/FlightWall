# Security and firmware signing

## Signing keys

Never commit a private firmware-signing key to this repository. Release builds
accept the key path through `FLIGHTWALL_SIGNING_KEY`; the key should live in a
secure location outside the working tree.

The public Raspberry Pi Pico SDK example key is not a production trust key.
Because its private half is publicly available, a signature made with that key
does not establish who produced the firmware.

FlightWall v1.6.0 and later authenticate automatic updates in two stages:

1. The firmware verifies `update/manifest.sig` against the embedded FlightWall
   secp256k1 public key.
2. It streams the inactive-slot UF2 while calculating SHA-256, and activates
   the image only when that digest matches the authenticated manifest.

The private key is also used by `picotool` to sign RP2350 images. Manual web
uploads remain an explicit local-administrator action; without OTP secure boot,
the RP2350 does not hardware-enforce the identity of manually uploaded images.

Before enabling hardware-enforced secure boot:

1. Generate a unique FlightWall signing key outside the repository.
2. Embed or provision the corresponding public trust information.
3. Test interrupted downloads, invalid signatures, and rollback behavior.
4. Keep a physical USB recovery path available.

RP2350 secure-boot configuration uses one-time-programmable settings. Do not
provision those settings until the complete signing, recovery, and key-backup
process has been tested on a nonessential device.

## Reporting a vulnerability

Until a private reporting address is published, avoid opening a public issue
that contains credentials, private keys, or exploitable device details. Contact
the repository owner privately through GitHub instead.
