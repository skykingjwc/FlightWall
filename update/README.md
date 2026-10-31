# Signed update feed

FlightWall reads this directory through the public GitHub REST API at
`api.github.com`. It does not require a GitHub token because the repository is
public.

- `manifest.json` describes the current release using schema 2.
- `manifest.sig` is an ECDSA/secp256k1 signature over the exact manifest bytes.
- `flightwall.uf2` is the inactive-slot OTA payload referenced by its Git blob SHA.

The manifest is authenticated with the public key compiled into
`update_public_key.h`. The private key must remain outside the repository.

Schema 2 includes both:

- `sha256`: verifies the decoded firmware payload before booting it.
- `blob_sha`: identifies the exact Git blob fetched from
  `/repos/skykingjwc/FlightWall/git/blobs/{blob_sha}`.

Create all three release files from a signed full Stage 3 UF2:

```sh
python3 tools/make_release.py \
  --version 1.6.21 \
  --build 10621 \
  --uf2 build/hub75_demo.uf2 \
  --private-key /secure/path/FlightWall-firmware-signing-private.pem
```

The script removes the leading partition-table block expected only by USB
installation, pads the application stream to the OTA receiver's 2 KiB boundary,
computes both the SHA-256 digest and deterministic Git blob SHA, signs the
manifest, and verifies the signature before returning success.
