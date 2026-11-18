#!/usr/bin/env python3
"""Import native MM6-MM8 textures and sprites from the player's purchased LODs.

The source archive cannot redistribute this art. By default this tool writes the
decoded x1 images below the ignored output/ folder, never into source or build
trees. Pass --output explicitly when preparing a separate development overlay.
"""

from __future__ import annotations

import argparse
import dataclasses
import pathlib
import re
import struct
import sys
import zlib


LOD_DIRECTORY_OFFSET = 0x120
LOD_ENTRY_SIZE = 32
MAX_IMAGE_DIMENSION = 4096


@dataclasses.dataclass(frozen=True)
class LodEntry:
    name: str
    payload: bytes


def u16(data: bytes, offset: int) -> int:
    return struct.unpack_from("<H", data, offset)[0]


def u32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def read_lod(path: pathlib.Path) -> list[LodEntry]:
    data = path.read_bytes()
    if len(data) < LOD_DIRECTORY_OFFSET or data[:3] != b"LOD":
        raise ValueError(f"{path} is not a supported LOD")

    payload_base = u32(data, 0x110)
    entry_count = u32(data, 0x11C)
    directory_end = LOD_DIRECTORY_OFFSET + entry_count * LOD_ENTRY_SIZE
    if payload_base > len(data) or directory_end > len(data):
        raise ValueError(f"{path} has an invalid directory")

    entries: list[LodEntry] = []
    for index in range(entry_count):
        entry_offset = LOD_DIRECTORY_OFFSET + index * LOD_ENTRY_SIZE
        record = data[entry_offset : entry_offset + LOD_ENTRY_SIZE]
        encoded_name = record[:16].split(b"\0", 1)[0]
        try:
            name = encoded_name.decode("ascii").lower()
        except UnicodeDecodeError:
            continue
        relative_offset, payload_size = struct.unpack_from("<II", record, 16)
        start = payload_base + relative_offset
        end = start + payload_size
        if not name or start < payload_base or end > len(data):
            continue
        entries.append(LodEntry(name, data[start:end]))
    return entries


def valid_image_size(width: int, height: int) -> bool:
    return 0 < width <= MAX_IMAGE_DIMENSION and 0 < height <= MAX_IMAGE_DIMENSION


def grayscale_palette() -> bytes:
    return b"".join(bytes((value, value, value, 0)) for value in range(256))


def bmp_bytes(width: int, height: int, pixels: bytes, palette_rgb: bytes | None = None) -> bytes:
    if len(pixels) != width * height:
        raise ValueError("pixel count does not match image dimensions")

    if palette_rgb is None or len(palette_rgb) < 256 * 3:
        palette = grayscale_palette()
    else:
        palette = b"".join(
            bytes((palette_rgb[index + 2], palette_rgb[index + 1], palette_rgb[index], 0))
            for index in range(0, 256 * 3, 3)
        )

    stride = (width + 3) & ~3
    pixel_offset = 14 + 40 + len(palette)
    pixel_size = stride * height
    header = bytearray(pixel_offset)
    struct.pack_into("<2sIHHI", header, 0, b"BM", pixel_offset + pixel_size, 0, 0, pixel_offset)
    struct.pack_into(
        "<IiiHHIIiiII", header, 14, 40, width, height, 1, 8, 0, pixel_size, 2835, 2835, 256, 256
    )
    header[54 : 54 + len(palette)] = palette

    rows = bytearray(pixel_size)
    for y in range(height):
        source = y * width
        target = (height - 1 - y) * stride
        rows[target : target + width] = pixels[source : source + width]
    return bytes(header) + bytes(rows)


def decode_sprite(payload: bytes) -> tuple[int, int, bytes]:
    if len(payload) < 32:
        raise ValueError("short sprite header")
    width = u16(payload, 16)
    height = u16(payload, 18)
    packed_size = u32(payload, 28)
    if not valid_image_size(width, height) or 32 + height * 8 > len(payload):
        raise ValueError("invalid sprite dimensions")

    encoded = payload[32 + height * 8 :]
    if encoded[:1] == b"x":
        packed = zlib.decompress(encoded)
    else:
        packed = encoded[:packed_size]
    if len(packed) < packed_size:
        raise ValueError("short sprite pixel stream")

    pixels = bytearray(width * height)
    for y in range(height):
        span_offset = 32 + y * 8
        left = u16(payload, span_offset)
        right = u16(payload, span_offset + 2)
        source = u32(payload, span_offset + 4)
        if left == 0xFFFF or right < left:
            continue
        if right >= width or source + right - left + 1 > len(packed):
            raise ValueError("invalid sprite row span")
        count = right - left + 1
        pixels[y * width + left : y * width + left + count] = packed[source : source + count]
    return width, height, bytes(pixels)


def decode_bitmap(payload: bytes) -> tuple[int, int, bytes, bytes]:
    if len(payload) < 48:
        raise ValueError("short bitmap header")
    top_level_size = u32(payload, 16)
    encoded_size = u32(payload, 20)
    width = u16(payload, 24)
    height = u16(payload, 26)
    mip_size = u32(payload, 40)
    if not valid_image_size(width, height) or top_level_size < width * height:
        raise ValueError("invalid bitmap dimensions")

    encoded_end = 48 + encoded_size
    if encoded_end > len(payload):
        raise ValueError("short bitmap pixel stream")
    encoded = payload[48:encoded_end]
    if encoded[:1] == b"x":
        pixels_with_mips = zlib.decompress(encoded)
    else:
        pixels_with_mips = encoded
    if len(pixels_with_mips) < max(width * height, mip_size):
        raise ValueError("short decoded bitmap pixel stream")

    palette = payload[encoded_end : encoded_end + 256 * 3]
    return width, height, pixels_with_mips[: width * height], palette


def safe_asset_name(name: str) -> str | None:
    stem = pathlib.PurePath(name).stem.lower()
    return stem if re.fullmatch(r"[a-z0-9_.-]+", stem) else None


def write_asset(path: pathlib.Path, data: bytes, preexisting: set[pathlib.Path], overwrite: bool) -> bool:
    key = path.resolve()
    if key in preexisting and not overwrite:
        return False
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    return True


def import_lod(
    lod_path: pathlib.Path,
    target: pathlib.Path,
    kind: str,
    preexisting: set[pathlib.Path],
    overwrite: bool,
    selected_names: set[str],
) -> tuple[int, int]:
    imported = 0
    skipped = 0
    decoder = decode_sprite if kind == "sprite" else decode_bitmap
    for entry in read_lod(lod_path):
        name = safe_asset_name(entry.name)
        if name is None or (selected_names and name not in selected_names):
            continue
        try:
            decoded = decoder(entry.payload)
            if kind == "sprite":
                width, height, pixels = decoded
                output = bmp_bytes(width, height, pixels)
            else:
                width, height, pixels, palette = decoded
                output = bmp_bytes(width, height, pixels, palette)
        except (ValueError, zlib.error):
            skipped += 1
            continue
        if write_asset(target / f"{name}.bmp", output, preexisting, overwrite):
            imported += 1
    return imported, skipped


def default_game_paths(root: pathlib.Path) -> list[tuple[str, pathlib.Path]]:
    return [
        ("MM6", root / "Might and Magic 6" / "data"),
        ("MM7", root / "Might and Magic 7" / "DATA"),
        ("MM8", root / "Might and Magic 8" / "Data"),
    ]


def main() -> int:
    script_root = pathlib.Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--games-root", type=pathlib.Path, default=script_root.parent)
    parser.add_argument("--output", type=pathlib.Path, default=script_root / "output" / "original_visuals")
    parser.add_argument("--overwrite-existing", action="store_true")
    parser.add_argument(
        "--name", action="append", default=[], help="Import only this case-insensitive asset name (repeatable)."
    )
    args = parser.parse_args()

    selected_names = {pathlib.PurePath(name).stem.lower() for name in args.name}
    sprite_target = args.output / "sprites"
    texture_target = args.output / "textures"
    preexisting = {
        path.resolve()
        for directory in (sprite_target, texture_target)
        if directory.exists()
        for path in directory.glob("*.bmp")
    }

    sources = default_game_paths(args.games_root.resolve())
    missing = [label for label, directory in sources if not (directory / "SPRITES.LOD").is_file()
        or not (directory / "BITMAPS.LOD").is_file()]
    if missing:
        parser.error("missing purchased game LODs for " + ", ".join(missing))

    totals = {"sprites": 0, "textures": 0, "skipped": 0}
    for label, directory in sources:
        sprite_lod = next(path for path in directory.iterdir() if path.name.lower() == "sprites.lod")
        bitmap_lod = next(path for path in directory.iterdir() if path.name.lower() == "bitmaps.lod")
        sprites, sprite_skips = import_lod(
            sprite_lod, sprite_target, "sprite", preexisting, args.overwrite_existing, selected_names
        )
        textures, texture_skips = import_lod(
            bitmap_lod, texture_target, "bitmap", preexisting, args.overwrite_existing, selected_names
        )
        totals["sprites"] += sprites
        totals["textures"] += textures
        totals["skipped"] += sprite_skips + texture_skips
        print(f"{label}: {sprites} sprites, {textures} textures ({sprite_skips + texture_skips} unsupported)")

    print(
        f"Imported {totals['sprites']} sprites and {totals['textures']} textures into {args.output}"
        f"; skipped {totals['skipped']} unsupported records."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
