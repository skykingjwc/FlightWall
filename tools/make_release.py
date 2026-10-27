#!/usr/bin/env python3

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import tempfile

UF2_MAGIC_START0 = 0x0A324655
UF2_MAGIC_START1 = 0x9E5D5157
UPDATE_PATH = "/skykingjwc/FlightWall/main/update/flightwall.uf2"


def run(command: list[str]) -> None:
    subprocess.run(command, check=True)


def main() -> int:
    parser = argparse.ArgumentParser(description="Build the signed FlightWall update feed")
    parser.add_argument("--version", required=True)
    parser.add_argument("--build", required=True, type=int)
    parser.add_argument("--uf2", required=True, type=Path)
    parser.add_argument("--private-key", required=True, type=Path)
    parser.add_argument("--output-dir", type=Path, default=Path("update"))
    args = parser.parse_args()

    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", args.version):
        parser.error("--version must use major.minor.patch")
    if args.build <= 0:
        parser.error("--build must be positive")
    if not args.uf2.is_file() or not args.private_key.is_file():
        parser.error("the UF2 and private key must exist")
    if os.stat(args.private_key).st_mode & 0o077:
        parser.error("private key permissions must not allow group or other access")

    full_image = args.uf2.read_bytes()
    if len(full_image) < 1024 or len(full_image) % 512:
        parser.error("the input is not a complete UF2")
    magic0, magic1 = struct.unpack_from("<II", full_image)
    if magic0 != UF2_MAGIC_START0 or magic1 != UF2_MAGIC_START1:
        parser.error("UF2 magic is invalid")

    payload = bytearray(full_image[512:])
    payload.extend(b"\x00" * ((-len(payload)) % 2048))
    digest = hashlib.sha256(payload).hexdigest()

    args.output_dir.mkdir(parents=True, exist_ok=True)
    firmware_path = args.output_dir / "flightwall.uf2"
    manifest_path = args.output_dir / "manifest.json"
    signature_path = args.output_dir / "manifest.sig"
    firmware_path.write_bytes(payload)

    manifest = {
        "build": args.build,
        "path": UPDATE_PATH,
        "schema": 1,
        "sha256": digest,
        "size": len(payload),
        "version": args.version,
    }
    manifest_bytes = (json.dumps(manifest, sort_keys=True, separators=(",", ":")) + "\n").encode()
    manifest_path.write_bytes(manifest_bytes)
    run(["openssl", "dgst", "-sha256", "-sign", str(args.private_key),
         "-out", str(signature_path), str(manifest_path)])

    with tempfile.TemporaryDirectory() as temporary_directory:
        public_key = Path(temporary_directory) / "public.pem"
        run(["openssl", "pkey", "-in", str(args.private_key), "-pubout",
             "-out", str(public_key)])
        run(["openssl", "dgst", "-sha256", "-verify", str(public_key),
             "-signature", str(signature_path), str(manifest_path)])

    print(f"release {args.version} build {args.build}")
    print(f"payload {len(payload)} bytes")
    print(f"sha256 {digest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
