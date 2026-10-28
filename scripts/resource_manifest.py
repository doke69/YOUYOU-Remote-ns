#!/usr/bin/env python3
"""Resource-mode validation and build manifest for UURemote-ns.

Only known source/resource paths are inspected. signing.key, user config,
session/token files and logs are never read or hashed.
"""
import argparse
import hashlib
import json
import os
import sys
from pathlib import Path

PRIVATE_UI = [
    "account.rgba", "arrow.rgba", "attachment.rgba", "back.rgba",
    "device.rgba", "display.rgba", "exit.rgba", "fps.rgba",
    "gamepad.rgba", "guide.rgba", "keyboard.rgba", "more.rgba",
    "picture.rgba", "radio.rgba", "radio_empty.rgba", "refresh.rgba",
    "safety.rgba", "sound.rgba", "windows.rgba",
]
OWN_UI = ["youyou-mark.rgba", "touch.rgba", "mouse.rgba"]
FONTS = ["NotoSansSC-Regular.otf", "NotoSansSC-Bold.otf", "OFL.txt"]
SHADERS = ["nv12.vert", "nv12.frag", "ui.vert", "ui.frag"]
LICENSES = [
    "deko3d-zlib.txt", "dekotools-GPL-3.0.txt", "json-MIT.txt",
    "libdatachannel-MPL-2.0.txt", "libjuice-MPL-2.0.txt",
    "libsrtp-BSD-3-Clause.txt", "mbedtls-Apache-2.0.txt",
    "plog-MIT.txt", "qrcodegen-notices.md", "usrsctp-BSD-3-Clause.txt",
]
ICON = "assets/youyou-icon.jpg"
EXPECTED_UI = set(PRIVATE_UI) | set(OWN_UI)


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def missing(root, rels):
    return [r for r in rels if not (root / r).is_file()]


def verify(mode, root, asset_dir=None, romfs_dir=None):
    errors = []
    if mode == "FULL":
        if not asset_dir:
            errors.append("FULL requires --asset-dir")
        else:
            for name in PRIVATE_UI:
                if not (asset_dir / name).is_file():
                    errors.append(f"missing private UI: {name}")
    for name in FONTS:
        if not (root / "assets/fonts" / name).is_file():
            errors.append(f"missing font: assets/fonts/{name}")
    for name in LICENSES:
        if not (root / "licenses" / name).is_file():
            errors.append(f"missing license: licenses/{name}")
    for name in SHADERS:
        if not (root / "shaders" / name).is_file():
            errors.append(f"missing shader source: shaders/{name}")
    if not (root / ICON).is_file():
        errors.append(f"missing main icon: {ICON}")
    for name in OWN_UI:
        if not (root / "assets" / name).is_file():
            errors.append(f"missing own UI: assets/{name}")
    if romfs_dir is not None:
        ui = romfs_dir / "ui-tv"
        expected = EXPECTED_UI if mode == "FULL" else set(OWN_UI)
        actual = {p.name for p in ui.glob("*.rgba")} if ui.is_dir() else set()
        missing_packed = expected - actual
        extra_packed = actual - expected
        if missing_packed:
            errors.append("missing packed UI: " + ", ".join(sorted(missing_packed)))
        if extra_packed:
            errors.append("unexpected packed UI: " + ", ".join(sorted(extra_packed)))
        if "brand.rgba" in actual:
            errors.append("brand.rgba must not be packed")
        # The actual packed bytes must match the intended source, including the
        # public own-asset overrides for the three shared names.
        for name in expected:
            if name in PRIVATE_UI and mode == "FULL":
                source = asset_dir / name
            else:
                source = root / "assets" / name
            packed = ui / name
            if source.is_file() and packed.is_file() and sha256(source) != sha256(packed):
                errors.append(f"packed UI hash mismatch: {name}")
        fonts_dir = romfs_dir / "fonts"
        for name in FONTS:
            source = root / "assets/fonts" / name
            packed = fonts_dir / name
            if not packed.is_file():
                errors.append(f"missing packed font: {name}")
            elif packed.stat().st_size == 0:
                errors.append(f"empty packed font: {name}")
            elif source.is_file() and sha256(source) != sha256(packed):
                errors.append(f"packed font hash mismatch: {name}")
        shaders_dir = romfs_dir / "shaders"
        for name in SHADERS:
            packed = shaders_dir / (name + ".dksh")
            if not packed.is_file():
                errors.append(f"missing packed shader: {name}.dksh")
            elif packed.stat().st_size == 0:
                errors.append(f"empty packed shader: {name}.dksh")
    if errors:
        return 1, errors
    return 0, []


def controlled_files(root):
    files = []
    for pattern in ("CMakeLists.txt", "source/*.cpp", "source/*.inc", "include/**/*.hpp",
                    "include/**/*.h", "shaders/*", "assets/fonts/*", "licenses/*",
                    "scripts/resource_manifest.py", "scripts/fix_nacp_title.py",
                    "cmake/**/*.cmake", "scripts/dependency-overrides/**/*"):
        for p in sorted(root.glob(pattern)):
            if p.is_file():
                files.append(p)
    for name in OWN_UI:
        files.append(root / "assets" / name)
    files.append(root / ICON)
    files.append(root / "romfs/default-config.ini")
    # Never include signing.key, external builds, user config, session, logs.
    return [p for p in files if "signing.key" not in p.as_posix()]


def manifest(args):
    root = Path(args.root)
    romfs = Path(args.romfs_dir)
    resources = {}
    for p in sorted((romfs / "ui-tv").glob("*.rgba")):
        resources[f"ui-tv/{p.name}"] = sha256(p)
    for p in sorted((romfs / "fonts").glob("*")):
        resources[f"fonts/{p.name}"] = sha256(p)
    for p in sorted((romfs / "shaders").glob("*.dksh")):
        resources[f"shaders/{p.name}"] = sha256(p)
    sources = {p.relative_to(root).as_posix(): sha256(p) for p in controlled_files(root)}
    options = {}
    for item in args.option or []:
        if "=" not in item:
            raise SystemExit(f"bad option: {item}")
        key, value = item.split("=", 1)
        options[key] = value
    output = {
        "format": 1,
        "version": args.version,
        "build_label": args.label,
        "resource_mode": args.mode,
        "fallback": args.mode == "FALLBACK",
        "source_files": sources,
        "dependency_overlay_manifest": (
            sha256(root / "scripts/dependency-overrides/manifest.json")
            if (root / "scripts/dependency-overrides/manifest.json").is_file() else None),
        "build_options": options,
        "resources": resources,
        "nro": {"path": "UURemote-ns.nro", "sha256": sha256(args.nro),
                "size": Path(args.nro).stat().st_size},
        "elf": {"path": "UURemote-ns.elf", "sha256": sha256(args.elf),
                "size": Path(args.elf).stat().st_size},
    }
    Path(args.output).write_text(json.dumps(output, indent=2, sort_keys=True) + "\n",
                                 encoding="utf-8")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="command", required=True)
    v = sub.add_parser("verify")
    v.add_argument("--mode", choices=["FULL", "FALLBACK"], required=True)
    v.add_argument("--root", required=True)
    v.add_argument("--asset-dir")
    v.add_argument("--romfs-dir")
    m = sub.add_parser("manifest")
    m.add_argument("--mode", choices=["FULL", "FALLBACK"], required=True)
    m.add_argument("--root", required=True)
    m.add_argument("--romfs-dir", required=True)
    m.add_argument("--nro", required=True)
    m.add_argument("--elf", required=True)
    m.add_argument("--output", required=True)
    m.add_argument("--version", required=True)
    m.add_argument("--label", required=True)
    m.add_argument("--option", action="append", default=[])
    args = ap.parse_args(argv)
    if args.command == "verify":
        code, errors = verify(args.mode, Path(args.root),
                              Path(args.asset_dir) if args.asset_dir else None,
                              Path(args.romfs_dir) if args.romfs_dir else None)
        for error in errors:
            print(f"resource-verify: {error}", file=sys.stderr)
        return code
    return manifest(args)


if __name__ == "__main__":
    sys.exit(main())
