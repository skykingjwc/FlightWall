#!/usr/bin/env python3

import argparse
import hashlib
import json
from pathlib import Path
import re
import struct

UF2_MAGIC_START0 = 0x0A324655
UF2_MAGIC_START1 = 0x9E5D5157


def main() -> int:
    parser = argparse.ArgumentParser(description="Build the FlightWall update feed")
    parser.add_argument("--version", required=True)
    parser.add_argument("--build", required=True, type=int)
    parser.add_argument("--uf2", required=True, type=Path)
    parser.add_argument("--output-dir", type=Path, default=Path("update"))
    args = parser.parse_args()

    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", args.version):
        parser.error("--version must use major.minor.patch")
    if args.build <= 0:
        parser.error("--build must be positive")
    if not args.uf2.is_file():
        parser.error("the UF2 must exist")

    full_image = args.uf2.read_bytes()
    if len(full_image) < 1024 or len(full_image) % 512:
        parser.error("the input is not a complete UF2")
    magic0, magic1 = struct.unpack_from("<II", full_image)
    if magic0 != UF2_MAGIC_START0 or magic1 != UF2_MAGIC_START1:
        parser.error("UF2 magic is invalid")

    payload = bytearray(full_image[512:])
    payload.extend(b"\x00" * ((-len(payload)) % 2048))
    digest = hashlib.sha256(payload).hexdigest()
    blob_header = f"blob {len(payload)}\0".encode()
    blob_sha = hashlib.sha1(blob_header + payload).hexdigest()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    firmware_path = args.output_dir / "flightwall.uf2"
    manifest_path = args.output_dir / "manifest.json"
    check_path = args.output_dir / "check.json"
    payload_info_path = args.output_dir / "payload-info.json"
    firmware_path.write_bytes(payload)

    manifest = {
        "build": args.build,
        "schema": 3,
        "sha256": digest,
        "size": len(payload),
        "version": args.version,
    }
    manifest["blob_sha"] = blob_sha
    manifest_bytes = (json.dumps(manifest, sort_keys=True, separators=(",", ":")) + "\n").encode()
    manifest_path.write_bytes(manifest_bytes)

    check = dict(manifest)
    check_path.write_text(json.dumps(check, sort_keys=True, separators=(",", ":")) + "\n")
    payload_info_path.write_text(
        json.dumps({
            "build": args.build,
            "sha256": digest,
            "size": len(payload),
            "version": args.version,
        }, sort_keys=True, indent=2) + "\n"
    )

    print(f"release {args.version} build {args.build} schema 3")
    print(f"payload {len(payload)} bytes")
    print(f"sha256 {digest}")
    print(f"blob_sha {blob_sha}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
