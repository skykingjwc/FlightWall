# Security and firmware signing

## Signing keys

Never commit a private firmware-signing key to this repository. Release builds
accept the key path through `FLIGHTWALL_SIGNING_KEY`; the key should live in a
secure location outside the working tree.

The public Raspberry Pi Pico SDK example key is not a production trust key.
Because its private half is publicly available, a signature made with that key
does not establish who produced the firmware.

Before enabling unattended internet updates:

1. Generate a unique FlightWall signing key outside the repository.
2. Embed or provision the corresponding public trust information.
3. Make the updater verify every release before activating it.
4. Test interrupted downloads, invalid signatures, and rollback behavior.
5. Keep a physical USB recovery path available.

RP2350 secure-boot configuration uses one-time-programmable settings. Do not
provision those settings until the complete signing, recovery, and key-backup
process has been tested on a nonessential device.

## Reporting a vulnerability

Until a private reporting address is published, avoid opening a public issue
that contains credentials, private keys, or exploitable device details. Contact
the repository owner privately through GitHub instead.
