# Signed update feed

FlightWall reads this directory from `raw.githubusercontent.com`.

- `manifest.json` describes the current release using schema 1.
- `manifest.sig` is an ECDSA/secp256k1 signature over the exact manifest bytes.
- `flightwall.uf2` is the inactive-slot OTA payload named by the manifest.

The manifest is authenticated with the public key compiled into
`update_public_key.h`. The private key must remain outside the repository.

Create all three release files from a signed full Stage 3 UF2:

```sh
python3 tools/make_release.py \
  --version 1.6.0 \
  --build 10600 \
  --uf2 build/hub75_demo.uf2 \
  --private-key /secure/path/FlightWall-firmware-signing-private.pem
```

The script removes the leading partition-table block expected only by USB
installation, pads the application stream to the OTA receiver's 2 KiB boundary,
hashes it, signs the deterministic manifest, and verifies the signature before
returning success.
