"""
release.py - Ayudas para publicar una versión de Luma (las usa GitHub Actions)
=============================================================================
Comandos:
  python scripts/release.py check-version v0.2.0
      Comprueba que la etiqueta coincida con la versión del código
      (client/core/version.h). Así nunca se publica "v0.2.0" con el número viejo.

  python scripts/release.py manifest --tag v0.2.0 --exe build/bin/Release/LumaLauncher.exe --out dist
      Arma dist/latest.json (versión, fecha, huella SHA-256 del .exe, notas y
      versiones de Minecraft soportadas), dist/notes.md (el texto de la
      Release en GitHub) y copia el .exe a dist/.

  python scripts/release.py der2raw firma.der firma.sig
      OpenSSL guarda las firmas ECDSA en formato "DER" (con encabezados). El
      launcher (BCrypt de Windows) las quiere "crudas": los números r y s de
      32 bytes cada uno, uno detrás del otro. Esto las convierte.

Los datos que se escriben a mano para cada versión:
  release/minecraft.json        -> ["1.26.63", ...] versiones de Minecraft soportadas
  release/notes/<versión>.json  -> novedades en inglés, español y portugués
                                   (opcional: "image", una imagen del repo, y
                                   "author", la nota del autor en los 3 idiomas)
"""
import argparse
import datetime
import hashlib
import json
import pathlib
import re
import shutil
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
REPO = "IAOcd/Luma"


def code_version():
    text = (ROOT / "client" / "core" / "version.h").read_text(encoding="utf-8")
    match = re.search(r'kVersion\s*=\s*"([0-9]+\.[0-9]+\.[0-9]+)"', text)
    if not match:
        sys.exit("No se encontró kVersion en client/core/version.h")
    return match.group(1)


def cmd_check_version(args):
    tag = args.tag.lstrip("v")
    version = code_version()
    if tag != version:
        sys.exit(f"La etiqueta es v{tag} pero el código dice {version}: cambia client/core/version.h")
    print(f"Versión {version}: OK")


def cmd_manifest(args):
    version = args.tag.lstrip("v")
    exe = pathlib.Path(args.exe)
    out = pathlib.Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    data = exe.read_bytes()
    notes_file = ROOT / "release" / "notes" / f"{version}.json"
    if not notes_file.exists():
        sys.exit(f"Faltan las notas de la versión: {notes_file}")
    notes = json.loads(notes_file.read_text(encoding="utf-8"))
    minecraft = json.loads((ROOT / "release" / "minecraft.json").read_text(encoding="utf-8"))
    manifest = {
        "version": version,
        "date": notes.get("date") or datetime.date.today().strftime("%d.%m.%Y"),
        "url": f"https://github.com/{REPO}/releases/download/v{version}/LumaLauncher.exe",
        "sha256": hashlib.sha256(data).hexdigest(),
        "size": len(data),
        "minecraft": minecraft,
        "notes": notes["notes"],
    }
    # Sin espacios raros ni saltos de línea de Windows: la firma se hace sobre
    # estos bytes exactos, y el launcher los compara tal cual.
    (out / "latest.json").write_bytes(json.dumps(manifest, ensure_ascii=False, indent=2).encode("utf-8"))
    shutil.copyfile(exe, out / "LumaLauncher.exe")
    lines = [f"## Luma v{version}", ""]
    # An image for the release (optional): a file of the repo, shown from the tag.
    if notes.get("image"):
        lines.append(f"![Luma v{version}](https://raw.githubusercontent.com/{REPO}/v{version}/{notes['image']})")
        lines.append("")
    for lang, title in (("en", "English"), ("es", "Español"), ("pt", "Português")):
        lines.append(f"### {title}")
        for note in notes["notes"].get(lang, []):
            lines.append(f"- **{note['title']}**: {note['body']}")
        lines.append("")
    # The author's note (optional), in the three languages.
    author = notes.get("author") or {}
    if author:
        for lang, title in (("en", "Author's note"), ("es", "Nota del autor"), ("pt", "Nota do autor")):
            if author.get(lang):
                lines.append(f"### {title}")
                lines.append(f"> {author[lang]}")
                lines.append("")
    lines.append("Minecraft: " + ", ".join(minecraft))
    lines.append("")
    lines.append(f"SHA-256 `LumaLauncher.exe`: `{manifest['sha256']}`")
    (out / "notes.md").write_text("\n".join(lines), encoding="utf-8")
    print(json.dumps({k: manifest[k] for k in ("version", "sha256", "size")}))


def read_der_length(data, i):
    first = data[i]
    if first < 0x80:
        return first, i + 1
    count = first & 0x7F
    return int.from_bytes(data[i + 1:i + 1 + count], "big"), i + 1 + count


def cmd_der2raw(args):
    der = pathlib.Path(args.input).read_bytes()
    # SEQUENCE { INTEGER r, INTEGER s }
    if der[0] != 0x30:
        sys.exit("La firma no es DER")
    _, i = read_der_length(der, 1)
    parts = []
    for _ in range(2):
        if der[i] != 0x02:
            sys.exit("La firma no es DER (falta un INTEGER)")
        length, i = read_der_length(der, i + 1)
        number = der[i:i + length].lstrip(b"\x00")
        if len(number) > 32:
            sys.exit("Número demasiado grande para P-256")
        parts.append(number.rjust(32, b"\x00"))
        i += length
    pathlib.Path(args.output).write_bytes(parts[0] + parts[1])
    print("firma cruda: 64 bytes")


def main():
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("check-version")
    p.add_argument("tag")
    p.set_defaults(func=cmd_check_version)
    p = sub.add_parser("manifest")
    p.add_argument("--tag", required=True)
    p.add_argument("--exe", required=True)
    p.add_argument("--out", required=True)
    p.set_defaults(func=cmd_manifest)
    p = sub.add_parser("der2raw")
    p.add_argument("input")
    p.add_argument("output")
    p.set_defaults(func=cmd_der2raw)
    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
