#!/usr/bin/env python3
from pathlib import Path
import hashlib
import json
import struct

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "airlines" / "source"
GENERATED = ROOT / "airlines" / "generated"
WIDTH = 50
HEIGHT = 22
SCHEMA = 1
MAGIC = 0x46574150
MAX_ICONS = 47

pngs = sorted(SOURCE.glob("*.png"))
if len(pngs) > MAX_ICONS:
    raise SystemExit(f"Too many icons: {len(pngs)} > {MAX_ICONS}")

entries = []
pixel_blobs = []
for path in pngs:
    prefix = path.stem.upper()
    if len(prefix) != 3 or not prefix.isalnum() or not prefix.isascii():
        raise SystemExit(f"{path.name}: filename must be a three-character ASCII callsign prefix")

    image = Image.open(path).convert("RGBA")
    if image.size != (WIDTH, HEIGHT):
        raise SystemExit(f"{path.name}: expected {WIDTH}x{HEIGHT}, got {image.width}x{image.height}")

    pixels = bytearray()
    for r, g, b, a in image.getdata():
        if a < 128:
            pixels.append(0)
            continue
        packed = ((r * 7 // 255) << 5) | ((g * 7 // 255) << 2) | (b * 3 // 255)
        pixels.append(packed if packed else 1)

    entries.append(prefix)
    pixel_blobs.append(bytes(pixels))

header_size = 16
entry_size = 8
data_offset = header_size + len(entries) * entry_size
payload = bytearray(struct.pack(
    "<IHHBBHI", MAGIC, SCHEMA, len(entries), WIDTH, HEIGHT, 0, data_offset
))

offset = data_offset
for prefix, pixels in zip(entries, pixel_blobs):
    payload.extend(struct.pack("<4sI", (prefix + "\0").encode("ascii"), offset))
    offset += len(pixels)
for pixels in pixel_blobs:
    payload.extend(pixels)

payload.extend(b"\x00" * ((-len(payload)) % 256))
if len(payload) > 52 * 1024:
    raise SystemExit(f"Pack is too large: {len(payload)} bytes")

GENERATED.mkdir(parents=True, exist_ok=True)
(GENERATED / "pack.bin").write_bytes(payload)
sha256 = hashlib.sha256(payload).hexdigest()
blob_sha = hashlib.sha1(f"blob {len(payload)}\0".encode() + payload).hexdigest()
manifest = {"blob_sha": blob_sha, "schema": SCHEMA, "sha256": sha256, "size": len(payload)}
(GENERATED / "check.json").write_text(
    json.dumps(manifest, sort_keys=True, separators=(",", ":")) + "\n"
)
(GENERATED / "pack-info.json").write_text(
    json.dumps({**manifest, "icons": entries, "width": WIDTH, "height": HEIGHT},
               sort_keys=True, indent=2) + "\n"
)
print(f"icons={len(entries)}")
print(f"size={len(payload)}")
print(f"sha256={sha256}")
print(f"blob_sha={blob_sha}")
