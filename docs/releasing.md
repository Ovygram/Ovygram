# Releasing Ovygram

A release is tagged `v<ovygram>-<telegram>`, e.g. `v0.1.0-7.4.6`, and ships
the sources with these assets:

| Asset | Made by |
| --- | --- |
| Windows x64 ZIP, Setup EXE and their `.sha256` files | GitHub Actions, `ovygram-windows.yml` in `plugins` mode |
| `ovygram-<version>-linux-x86_64.tar.xz` and `.sha256` | `tools/ovygram/package_linux.py` on Arch Linux |

The versions are `OVYGRAM_VERSION` and `CLIENT_VERSION` in
`tools/ovygram/baseline.py`; the About box shows the same version.

Ovygram uses its own signed manifest and Windows updater. The Windows Setup
EXE provides installation for the current user.

## Linux

The Linux build links the system libraries of the machine it is built on
(Qt 6, FFmpeg and the others from the Arch repositories), so the archive is
meant for Arch Linux and the `ovygram-bin` package.

1. Stage the Python bundle (`tools/ovygram/python_bundle.py`, it includes
   the pip wheel) and build Release as in
   [building-ovygram.md](building-ovygram.md#linux-x64).
2. Pack: `python tools/ovygram/package_linux.py --build out/Release`. The
   script checks the Python bundle and widget helper, strips the Ovygram
   binary and writes the archive and its `.sha256` to `dist/`.
3. If the build machine's libraries changed, refresh the dependencies:
   `python tools/ovygram/arch_depends.py out/Release` and update `depends`
   in `packaging/arch/ovygram-bin/PKGBUILD` (keep the runtime-only ones:
   `hicolor-icon-theme`, `pipewire`, `qt6-imageformats`).

The application ID is `io.github.maxutka00.Ovygram`: the `.desktop` file,
the icons, the Wayland app ID and the D-Bus name. Ovygram keeps its data in
`~/.local/share/Ovygram` and never reads `~/.TelegramDesktop`, so it runs
next to the official Telegram Desktop.

## Windows

Run `ovygram-windows.yml` with `mode: plugins` on the commit being released.
The repository's GitHub Actions secrets `TDESKTOP_API_ID` and
`TDESKTOP_API_HASH` configure the Telegram API for the release build.

The `ovygram-<run_id>-with-setup` artifact includes a portable ZIP, a per-user
Inno Setup installer and a SHA-256 file for each. The installer stages the same
binaries, modules and Python bundle as the ZIP. It installs to
`%APPDATA%\Ovygram` by default and preserves user data during uninstall.

Open `/updates` in the admin bot, select the run and channel, and confirm
publication. The bot verifies the checksums and publishes both files on the
same GitHub Release. The signed auto-update manifest points to the ZIP.

## Publishing

1. Go through [the client checklist](ui-checklist.md) on both systems.
2. Download the Windows artifact contents to `artifacts/`, then tag and
   publish (the draft notes are in `docs/release-notes/`):

   ```
   git tag -a v0.1.0-7.4.6 -m "Ovygram 0.1.0 (Telegram Desktop 7.4.6)"
   git push origin v0.1.0-7.4.6
   gh release create v0.1.0-7.4.6 --notes-file docs/release-notes/0.1.0-7.4.6.md \
       dist/ovygram-0.1.0-7.4.6-linux-x86_64.tar.xz \
       dist/ovygram-0.1.0-7.4.6-linux-x86_64.tar.xz.sha256 \
       artifacts/ovygram-0.1.0-7.4.6-win64-release.zip \
       artifacts/ovygram-0.1.0-7.4.6-win64-release.zip.sha256 \
       artifacts/ovygram-0.1.0-7.4.6-win64-setup.exe \
       artifacts/ovygram-0.1.0-7.4.6-win64-setup.exe.sha256
   ```

3. AUR: in `packaging/arch/ovygram-bin`, put the archive's SHA-256 into
   `sha256sums` (or run `updpkgsums`), check with `makepkg -f`, regenerate
   `.SRCINFO` with `makepkg --printsrcinfo > .SRCINFO` and push both files to
   `ssh://aur@aur.archlinux.org/ovygram-bin.git`.

Before a public release the application also needs its own API credentials
and icon, see [development-status.md](development-status.md).
