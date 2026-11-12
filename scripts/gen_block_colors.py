"""
gen_block_colors.py - The color of every block, from the game's own textures
==============================================================================
The minimap paints each block with a color. Instead of a table written by
hand, this script reads the textures that come with YOUR Minecraft and takes
the average color of each block's top face (what a map sees from above).
The result is client/sdk/block_colors.inc: only numbers (names and colors),
no game files are copied into Luma.

  python scripts/gen_block_colors.py ["C:/XboxGames/Minecraft for Windows/Content/data/resource_packs"]

Run it again after a game update to get the colors of the new blocks.

How the game keeps them:
  - resource_packs/vanilla, vanilla_1.14, vanilla_1.16, ... : one pack per
    version; the newer ones add or replace blocks.
  - blocks.json: block -> texture names ("up" = the top face).
  - textures/terrain_texture.json: texture name -> file (and a tint color
    for some, like grass).
  - __brarchive/textures/blocks*.brarchive: the image files, packed in
    Mojang's simple archive format: a header (8-byte magic, count, version),
    then 256 bytes per file (name length, name, offset, size), then the data.
Grass, leaves and water are gray in their files: the game tints them by
biome. Here they get the "plains" tint.
"""
import io
import json
import pathlib
import re
import struct
import sys

from PIL import Image

DEFAULT = r"C:/XboxGames/Minecraft for Windows/Content/data/resource_packs"
OUT = pathlib.Path(__file__).resolve().parent.parent / "client" / "sdk" / "block_colors.inc"

# Default biome tints (plains) for textures that are gray in their files.
GRASS = (0x79, 0xC0, 0x5A)
FOLIAGE = (0x59, 0xAE, 0x30)
WATER = (0x44, 0xAF, 0xF5)
TINTED = [("grass", GRASS), ("tallgrass", GRASS), ("fern", GRASS), ("double_plant", GRASS), ("vine", FOLIAGE),
          ("leaves", FOLIAGE), ("waterlily", FOLIAGE), ("water", WATER), ("lily", FOLIAGE)]


def lenient_json(text):
    text = re.sub(r"//[^\n]*", "", text)
    text = re.sub(r",(\s*[}\]])", r"\1", text)
    return json.loads(text)


def version_key(name):
    nums = re.findall(r"\d+", name)
    return tuple(int(n) for n in nums) if nums else (0,)


def read_archive(path, prefix, files):
    data = path.read_bytes()
    _, count, _ = struct.unpack_from("<QII", data, 0)
    base = 16 + count * 256
    for i in range(count):
        off = 16 + i * 256
        n = data[off]
        name = data[off + 1:off + 1 + n].decode("utf-8", "replace")
        start, size = struct.unpack_from("<II", data, off + 248)
        files[prefix + name] = data[base + start:base + start + size]


def main():
    root = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else DEFAULT)
    packs = [p for p in root.iterdir() if p.is_dir() and (p.name == "vanilla" or re.fullmatch(r"vanilla_[\d.]+", p.name))]
    packs.sort(key=lambda p: (0,) if p.name == "vanilla" else version_key(p.name))
    blocks, textures, files = {}, {}, {}
    for pack in packs:
        if (pack / "blocks.json").exists():
            for k, v in lenient_json((pack / "blocks.json").read_text(encoding="utf-8")).items():
                if k != "format_version":
                    blocks[k] = v
        tt = pack / "textures" / "terrain_texture.json"
        if tt.exists():
            textures.update(lenient_json(tt.read_text(encoding="utf-8")).get("texture_data", {}))
        arch = pack / "__brarchive" / "textures"
        if (arch / "blocks.brarchive").exists():
            read_archive(arch / "blocks.brarchive", "textures/blocks/", files)
        if (arch / "blocks").is_dir():
            for sub in (arch / "blocks").glob("*.brarchive"):
                read_archive(sub, f"textures/blocks/{sub.stem}/", files)

    def image_color(path, tint=None):
        raw = files.get(path + ".png") or files.get(path + ".tga")
        if raw is None:
            return None
        try:
            img = Image.open(io.BytesIO(raw)).convert("RGBA")
        except Exception:
            return None
        if img.width > 64:  # animated strips (water, lava...): the first frame
            img = img.crop((0, 0, img.width, img.width)) if img.height > img.width else img
        r = g = b = w = 0.0
        for pr, pg, pb, pa in (img.get_flattened_data() if hasattr(img, "get_flattened_data") else img.getdata()):
            if pa < 24:
                continue
            a = pa / 255.0
            r += pr * a
            g += pg * a
            b += pb * a
            w += a
        if w == 0:
            return None
        c = [r / w, g / w, b / w]
        name = path.rsplit("/", 1)[-1]
        if tint is None:
            sat = max(c) - min(c)
            if sat < 18:  # gray in the file: probably tinted by biome
                for key, t in TINTED:
                    if key in name:
                        tint = t
                        break
        if tint is not None:
            c = [c[i] * tint[i] / 255.0 for i in range(3)]
        return tuple(max(0, min(255, int(round(x)))) for x in c)

    def first_texture(entry):
        tex = entry.get("textures") if isinstance(entry, dict) else entry
        tint = None
        while isinstance(tex, list):
            tex = tex[0] if tex else None
        if isinstance(tex, dict):
            col = tex.get("overlay_color") or tex.get("tint_color")
            if isinstance(col, str) and col.startswith("#") and len(col) >= 7:
                tint = tuple(int(col[i:i + 2], 16) for i in (1, 3, 5))
            tex = tex.get("path")
        return tex, tint

    def texture_color(tname):
        entry = textures.get(tname)
        if entry is None:
            return None
        path, tint = first_texture(entry)
        return image_color(path, tint) if isinstance(path, str) else None

    out_blocks = {}
    for name, spec in blocks.items():
        tex = spec.get("textures") if isinstance(spec, dict) else None
        if isinstance(tex, dict):
            tex = tex.get("up") or next(iter(tex.values()), None)
        if isinstance(tex, str):
            c = texture_color(tex)
            if c:
                out_blocks[name] = c
    # Every texture file too (for blocks whose modern name isn't in blocks.json:
    # Luma matches the name's words, like "orange_wool" -> "wool_colored_orange").
    out_textures = {}
    for path in sorted(files):
        if not path.startswith("textures/blocks/"):
            continue
        stem = path[len("textures/blocks/"):].rsplit(".", 1)[0]
        c = image_color("textures/blocks/" + stem)
        if c:
            out_textures[stem.replace("/", "_")] = c

    lines = ["// Generated by scripts/gen_block_colors.py from the game's own textures (average color",
             "// of each block's top face). Only names and colors: no game files. Do not edit by hand.",
             "// " + f"{len(out_blocks)} blocks, {len(out_textures)} textures.",
             "inline constexpr BlockColor kBlockColors[] = {"]
    for k in sorted(out_blocks):
        r, g, b = out_blocks[k]
        lines.append(f'    {{"{k}", 0x{r:02X}{g:02X}{b:02X}}},')
    lines.append("};")
    lines.append("inline constexpr BlockColor kTextureColors[] = {")
    for k in sorted(out_textures):
        r, g, b = out_textures[k]
        lines.append(f'    {{"{k}", 0x{r:02X}{g:02X}{b:02X}}},')
    lines.append("};")
    OUT.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    print(f"{len(packs)} packs, {len(files)} files -> {len(out_blocks)} blocks, {len(out_textures)} textures -> {OUT}")


if __name__ == "__main__":
    main()
