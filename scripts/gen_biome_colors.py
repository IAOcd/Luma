"""
gen_biome_colors.py - The grass, leaves and water color of every biome
=======================================================================
In Minecraft grass, leaves and water are gray in their textures: the game
TINTS them with the color of the biome they're in. This script works out
that color for every biome, from YOUR game's own files, and writes
client/sdk/biome_colors.inc (only names and colors: no game files copied).

  python scripts/gen_biome_colors.py ["C:/XboxGames/Minecraft for Windows/Content/data"]

How the game decides the colors:
  - behavior_packs/vanilla*/__brarchive/biomes.brarchive: one JSON per biome.
    "minecraft:climate" has its temperature and downfall (rain). Some have
    "minecraft:map_tints": a fixed color for grass/foliage (cherry grove,
    swamp...).
  - resource_packs/vanilla*/__brarchive/textures/colormap.brarchive:
    grass.png and foliage.png, 256 x 256 "palettes". The color is the pixel
    at x = (1 - temperature) * 255, y = (1 - downfall * temperature) * 255
    (both clamped to 0..1) - Minecraft's classic rule.
  - resource_packs/vanilla/biomes_client.json: "water_surface_color" per biome.
Run it again after a game update to get the new biomes.
"""
import io
import json
import pathlib
import re
import struct
import sys

from PIL import Image

DEFAULT = r"C:/XboxGames/Minecraft for Windows/Content/data"
OUT = pathlib.Path(__file__).resolve().parent.parent / "client" / "sdk" / "biome_colors.inc"


def read_archive(path):
    data = path.read_bytes()
    _, count, _ = struct.unpack_from("<QII", data, 0)
    base = 16 + count * 256
    out = {}
    for i in range(count):
        off = 16 + i * 256
        n = data[off]
        name = data[off + 1:off + 1 + n].decode("utf-8", "replace")
        start, size = struct.unpack_from("<II", data, off + 248)
        out[name] = data[base + start:base + start + size]
    return out


def lenient_json(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    text = re.sub(r",(\s*[}\]])", r"\1", text)
    return json.loads(text)


def version_key(name):
    nums = re.findall(r"\d+", name)
    return tuple(int(n) for n in nums) if nums else (0,)


def packs(root, kind):
    ps = [p for p in (root / kind).iterdir() if p.is_dir() and (p.name == "vanilla" or re.fullmatch(r"vanilla_[\d.]+", p.name))]
    return sorted(ps, key=lambda p: (0,) if p.name == "vanilla" else version_key(p.name))


def hex_color(s):
    if isinstance(s, str) and s.startswith("#") and len(s) >= 7:
        return tuple(int(s[i:i + 2], 16) for i in (1, 3, 5))
    return None


def main():
    root = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else DEFAULT)
    climate, tints = {}, {}
    for pack in packs(root, "behavior_packs"):
        arch = pack / "__brarchive" / "biomes.brarchive"
        if not arch.exists():
            continue
        for _, raw in read_archive(arch).items():
            try:
                j = lenient_json(raw.decode("utf-8", "replace"))
            except Exception:
                continue
            b = j.get("minecraft:biome", {})
            ident = b.get("description", {}).get("identifier")
            comps = b.get("components", {})
            if not ident:
                continue
            c = comps.get("minecraft:climate")
            if isinstance(c, dict):
                climate[ident] = (float(c.get("temperature", 0.8)), float(c.get("downfall", 0.4)))
            mt = comps.get("minecraft:map_tints")
            if isinstance(mt, dict):
                tints[ident] = mt
    maps = {}
    for pack in packs(root, "resource_packs"):
        arch = pack / "__brarchive" / "textures" / "colormap.brarchive"
        if arch.exists():
            for name, raw in read_archive(arch).items():
                try:
                    maps[name] = Image.open(io.BytesIO(raw)).convert("RGB")
                except Exception:
                    pass
    water = {}
    for pack in packs(root, "resource_packs"):
        bc = pack / "biomes_client.json"
        if bc.exists():
            for k, v in lenient_json(bc.read_text(encoding="utf-8")).get("biomes", {}).items():
                col = hex_color(v.get("water_surface_color")) if isinstance(v, dict) else None
                if col:
                    water[k if ":" in k else "minecraft:" + k] = col
    default_water = water.get("minecraft:default", (0x44, 0xAF, 0xF5))

    def lookup(img_name, temp, down):
        img = maps.get(img_name)
        if img is None:
            return None
        t = min(1.0, max(0.0, temp))
        d = min(1.0, max(0.0, down)) * t
        x = int((1.0 - t) * 255)
        y = int((1.0 - d) * 255)
        return img.getpixel((min(255, x), min(255, y)))

    rows = []
    for ident in sorted(climate):
        temp, down = climate[ident]
        grass = lookup("grass.png", temp, down) or (0x79, 0xC0, 0x5A)
        foliage = lookup("foliage.png", temp, down) or (0x59, 0xAE, 0x30)
        mt = tints.get(ident, {})
        g = mt.get("grass")
        if isinstance(g, dict) and g.get("type") == "tint":
            grass = hex_color(g.get("tint")) or grass
        elif isinstance(g, str):
            grass = hex_color(g) or grass
        elif isinstance(g, dict) and g.get("type") == "noise" and "swamp_grass.png" in maps:
            grass = maps["swamp_grass.png"].getpixel((128, 128))  # (swamp: a noise between two greens; the middle one)
        f = mt.get("foliage")
        if isinstance(f, str):
            foliage = hex_color(f) or foliage
        w = water.get(ident, default_water)
        rows.append((ident.split(":", 1)[1], grass, foliage, w))

    def c(rgb):
        return "0x%02X%02X%02X" % rgb[:3]

    lines = ["// Generated by scripts/gen_biome_colors.py from the game's own files (biome climate,",
             "// map tints, colormaps and water colors). Only names and colors. Do not edit by hand.",
             f"// {len(rows)} biomes. {{name, grass, foliage, water}}",
             "inline constexpr BiomeColors kBiomeColors[] = {"]
    for name, g, f, w in rows:
        lines.append(f'    {{"{name}", {c(g)}, {c(f)}, {c(w)}}},')
    lines.append("};")
    OUT.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    print(f"{len(rows)} biomes, {len(maps)} colormaps, {len(water)} water colors -> {OUT}")


if __name__ == "__main__":
    main()
