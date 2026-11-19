"""Stage the pinned Xray-core Windows binary and its license."""

import hashlib
from pathlib import Path
import shutil
import subprocess
import sys
from tempfile import TemporaryDirectory
from urllib.request import urlopen
from zipfile import ZipFile


VERSION = "26.11.9"
SOURCE = f"https://github.com/XTLS/Xray-core/releases/download/v{VERSION}/Xray-windows-64.zip"
SHA256 = "1262522266337b824f07beb8e1375786b437683b3890105469088f8e17e0a69d"


def stage(target):
    target.mkdir(parents=True, exist_ok=True)
    with TemporaryDirectory() as directory:
        download = Path(directory) / "xray.zip"
        with urlopen(SOURCE, timeout=60) as response, download.open("wb") as output:
            shutil.copyfileobj(response, output)
        with download.open("rb") as stream:
            if hashlib.file_digest(stream, "sha256").hexdigest() != SHA256:
                raise RuntimeError("The Xray archive SHA-256 does not match.")
        with ZipFile(download) as archive:
            for name in ("xray.exe", "LICENSE"):
                with archive.open(name) as source, (target / name).open("wb") as output:
                    shutil.copyfileobj(source, output)

    version = subprocess.check_output([str(target / "xray.exe"), "version"], text=True, timeout=30)
    if not version.startswith(f"Xray {VERSION} "):
        raise RuntimeError("The staged Xray executable reported an unexpected version.")
    (target / "SOURCE.txt").write_text(
        f"Xray-core {VERSION}\nhttps://github.com/XTLS/Xray-core/tree/v{VERSION}\n"
        f"Release archive: {SOURCE}\nSHA-256: {SHA256}\n",
        encoding="utf-8",
    )
    print(version.splitlines()[0], flush=True)


if __name__ == "__main__":
    stage(Path(sys.argv[1]).resolve())
