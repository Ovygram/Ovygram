"""Pack a pinned official Xray release ZIP into a separate SDK 3 test plugin."""
import argparse
import hashlib
from pathlib import Path
from zipfile import ZipFile, ZipInfo, ZIP_DEFLATED

ROOT = Path(__file__).resolve().parent
RELEASE = "https://github.com/XTLS/Xray-core/tree/v26.11.9"
BINARIES = {
    "1262522266337b824f07beb8e1375786b437683b3890105469088f8e17e0a69d": "xray.exe",
    "b651f7e1d9966ee2c1b1ee1bf8b52c72771824d3065dce96bbc8bcc19ed95f82": "xray",
}


def add(package, name, data):
    # Fixed ZIP metadata makes the same sources and release produce the same package hash.
    entry = ZipInfo(name, (1980, 1, 1, 0, 0, 0))
    entry.create_system = 3
    entry.external_attr = 0o100644 << 16
    package.writestr(entry, data, compress_type=ZIP_DEFLATED)


def pack(release, destination):
    with release.open("rb") as source:
        digest = hashlib.file_digest(source, "sha256").hexdigest()
        binary = BINARIES.get(digest)
        if binary is None:
            raise ValueError("Unexpected release SHA-256")
        destination.mkdir(parents=True, exist_ok=True)
        with ZipFile(source) as upstream, ZipFile(destination / "vless_test.ovyzip", "w", ZIP_DEFLATED) as package:
            for name in ("main.py", "plugin.json"):
                add(package, name, (ROOT / name).read_bytes())
            add(package, "bin/" + binary, upstream.read(binary))
            add(package, "LICENSE-Xray", upstream.read("LICENSE"))
            add(package, "SOURCE.txt", f"Xray-core 26.11.9\n{RELEASE}\nRelease ZIP SHA-256 {digest}\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("release", type=Path)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()
    pack(args.release, args.destination)
