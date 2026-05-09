#!/usr/bin/env python3
"""Fetch the official MoltenVK macOS prebuilts for local Apple builds.

Homebrew's molten-vk bottle is not built with MoltenVK private API support.
The official release ships a macOS private-API tarball that avoids runtime
warnings around primitive restart on pipelines that disable it.

This script extracts the universal macOS dynamic library and the public
MoltenVK headers into third_party/MoltenVK/. That directory is intentionally
gitignored because it contains binary dependencies.
"""

import argparse
import hashlib
import shutil
import sys
import tarfile
import tempfile
import urllib.request
from pathlib import Path

DEFAULT_VERSION = "v1.4.1"

REPO_ROOT = Path(__file__).resolve().parent.parent
DEST_DIR = REPO_ROOT / "third_party" / "MoltenVK"


def fetch(version: str) -> None:
    macos_url = (
        "https://github.com/KhronosGroup/MoltenVK/releases/download/"
        f"{version}/MoltenVK-macos-privateapi.tar"
    )

    DEST_DIR.mkdir(parents=True, exist_ok=True)
    lib_dir = DEST_DIR / "lib"
    include_dir = DEST_DIR / "include" / "MoltenVK"
    lib_dir.mkdir(exist_ok=True)
    include_dir.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory() as tmp:
        tar_path = Path(tmp) / "MoltenVK-macos-privateapi.tar"
        print(f"Downloading {macos_url}")
        with urllib.request.urlopen(macos_url) as response, open(
            tar_path, "wb"
        ) as tar_file:
            shutil.copyfileobj(response, tar_file)

        print("Extracting macOS dylib and headers")
        with tarfile.open(tar_path, "r") as tar:
            dylib_member = tar.getmember(
                "MoltenVK/MoltenVK/dynamic/dylib/macOS/libMoltenVK.dylib"
            )
            with tar.extractfile(dylib_member) as src:
                if src is None:
                    raise RuntimeError("MoltenVK dylib is missing in tarball")
                (lib_dir / "libMoltenVK.dylib").write_bytes(src.read())

            for header_path in (
                "MoltenVK/MoltenVK/include/MoltenVK/mvk_private_api.h",
                "MoltenVK/MoltenVK/include/MoltenVK/mvk_vulkan.h",
            ):
                try:
                    member = tar.getmember(header_path)
                except KeyError:
                    continue
                with tar.extractfile(member) as src:
                    if src is None:
                        continue
                    (include_dir / Path(header_path).name).write_bytes(
                        src.read()
                    )

    (DEST_DIR / "VERSION").write_text(version + "\n")
    dylib_path = lib_dir / "libMoltenVK.dylib"
    digest = hashlib.sha256(dylib_path.read_bytes()).hexdigest()[:16]
    print(f"Installed {dylib_path} (sha256: {digest}...)")
    print("Re-run CMake/build to bundle the local dylib into the app.")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--version",
        default=DEFAULT_VERSION,
        help=f"MoltenVK release tag to fetch (default: {DEFAULT_VERSION}).",
    )
    args = parser.parse_args()
    try:
        fetch(args.version)
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
