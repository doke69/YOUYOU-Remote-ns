#!/usr/bin/env python3
"""Fixtures for the F10 resource verifier. No image pixels or real keys are read."""
import os
import shutil
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import resource_manifest as rm  # noqa: E402


def link_or_copy(src, dst):
    dst.parent.mkdir(parents=True, exist_ok=True)
    try:
        os.link(src, dst)
    except OSError:
        shutil.copy2(src, dst)


def build_fixture(base):
    root = base / "root"
    for name in rm.FONTS:
        link_or_copy(ROOT / "assets/fonts" / name, root / "assets/fonts" / name)
    for name in rm.LICENSES:
        link_or_copy(ROOT / "licenses" / name, root / "licenses" / name)
    for name in rm.SHADERS:
        link_or_copy(ROOT / "shaders" / name, root / "shaders" / name)
    for name in rm.OWN_UI:
        link_or_copy(ROOT / "assets" / name, root / "assets" / name)
    link_or_copy(ROOT / rm.ICON, root / rm.ICON)
    private = base / "private"
    private.mkdir()
    for name in rm.PRIVATE_UI:
        (private / name).write_bytes(b"fixture-" + name.encode())
    romfs = base / "romfs"
    ui = romfs / "ui-tv"
    ui.mkdir(parents=True)
    for name in rm.PRIVATE_UI:
        link_or_copy(private / name, ui / name)
    for name in rm.OWN_UI:
        link_or_copy(root / "assets" / name, ui / name)
    for name in rm.FONTS:
        link_or_copy(root / "assets/fonts" / name, romfs / "fonts" / name)
    for name in rm.SHADERS:
        (romfs / "shaders").mkdir(parents=True, exist_ok=True)
        (romfs / "shaders" / (name + ".dksh")).write_bytes(b"fixture-shader")
    return root, private, romfs, ui


def expect(mode, root, private, romfs, expected):
    code, errors = rm.verify(mode, root, private, romfs)
    if expected and code != 0:
        raise SystemExit(f"expected success, got {errors}")
    if not expected and code == 0:
        raise SystemExit("expected failure")


def remove_expect(path, mode, root, private, romfs):
    backup = path.with_suffix(path.suffix + ".bak")
    os.replace(path, backup)
    expect(mode, root, private, romfs, False)
    os.replace(backup, path)


def main():
    with tempfile.TemporaryDirectory(prefix="uu-f10-") as raw:
        base = Path(raw)
        root, private, romfs, ui = build_fixture(base)
        expect("FULL", root, private, romfs, True)
        expect("FALLBACK", root, private, romfs, False)  # full 22 not a fallback set

        # Remove every required source item, not just one per category.
        for name in rm.PRIVATE_UI:
            remove_expect(private / name, "FULL", root, private, romfs)
        for name in rm.FONTS:
            remove_expect(root / "assets/fonts" / name, "FULL", root, private, romfs)
        for name in rm.LICENSES:
            remove_expect(root / "licenses" / name, "FULL", root, private, romfs)
        for name in rm.SHADERS:
            remove_expect(root / "shaders" / name, "FULL", root, private, romfs)
        remove_expect(root / rm.ICON, "FULL", root, private, romfs)
        for name in rm.OWN_UI:
            remove_expect(root / "assets" / name, "FULL", root, private, romfs)

        # Remove every required packed item and corrupt bytes/size cases.
        for name in rm.PRIVATE_UI:
            remove_expect(ui / name, "FULL", root, private, romfs)
        for name in rm.OWN_UI:
            remove_expect(ui / name, "FULL", root, private, romfs)
        for name in rm.FONTS:
            remove_expect(romfs / "fonts" / name, "FULL", root, private, romfs)
        for name in rm.SHADERS:
            remove_expect(romfs / "shaders" / (name + ".dksh"), "FULL", root, private, romfs)

        target = ui / rm.PRIVATE_UI[0]
        target.unlink(); target.write_bytes(b"wrong-ui")
        expect("FULL", root, private, romfs, False)
        target.unlink(); link_or_copy(private / rm.PRIVATE_UI[0], target)

        font = romfs / "fonts" / rm.FONTS[0]
        font.unlink(); font.write_bytes(b"wrong-font")
        expect("FULL", root, private, romfs, False)
        font.unlink(); link_or_copy(root / "assets/fonts" / rm.FONTS[0], font)

        shader = romfs / "shaders" / (rm.SHADERS[0] + ".dksh")
        shader.write_bytes(b"")
        expect("FULL", root, private, romfs, False)

        # Fallback: only own UI, but still requires packed fonts/shaders.
        fallback_romfs = base / "fallback-romfs"
        fallback_ui = fallback_romfs / "ui-tv"
        fallback_ui.mkdir(parents=True)
        for name in rm.OWN_UI:
            link_or_copy(root / "assets" / name, fallback_ui / name)
        for name in rm.FONTS:
            link_or_copy(root / "assets/fonts" / name, fallback_romfs / "fonts" / name)
        for name in rm.SHADERS:
            (fallback_romfs / "shaders").mkdir(parents=True, exist_ok=True)
            (fallback_romfs / "shaders" / (name + ".dksh")).write_bytes(b"fixture-shader")
        expect("FALLBACK", root, private, fallback_romfs, True)
        (fallback_ui / rm.OWN_UI[0]).unlink()
        expect("FALLBACK", root, private, fallback_romfs, False)

        # Controlled source manifest must include inc/header/script/cmake inputs
        # and must not include secrets, user data, logs or external trees.
        controlled = {p.relative_to(ROOT).as_posix() for p in rm.controlled_files(ROOT)}
        for required in ("source/live_frontend.inc", "source/native_touch.inc",
                         "include/uu_mbedtls_config.h", "include/switch_compat/endian.h",
                         "include/switch_compat/sctp_layout.h",
                         "scripts/resource_manifest.py", "cmake/switch.cmake",
                         "romfs/default-config.ini"):
            if required not in controlled:
                raise SystemExit(f"controlled manifest missing {required}")
        for forbidden in controlled:
            low = forbidden.lower()
            if ("signing" in low or "connection.log" in low or "client-id" in low or
                    "session.json" in low or "ui-state" in low or low.startswith("external/") or
                    low.startswith(".git/")):
                raise SystemExit(f"sensitive path in manifest: {forbidden}")

    print("PASS: FULL/FALLBACK every required item, packed font/shader, hash/empty, controlled inputs, sensitive exclusion")
    return 0


if __name__ == "__main__":
    sys.exit(main())
