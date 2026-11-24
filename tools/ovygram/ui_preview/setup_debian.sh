#!/usr/bin/env bash
set -euo pipefail

# Install the build inputs into a writable directory on Debian 13 x86-64.
# /usr/bin/apt-get only downloads authenticated Debian packages.
source /etc/os-release
if [[ $ID != debian || $VERSION_ID != 13 || $(uname -m) != x86_64 ]]; then
    echo 'Automatic setup requires Debian 13 x86-64. See docs/ui-preview.md for system dependencies.' >&2
    exit 2
fi

mkdir -p "$1"
cache=$(cd -- "$1" && pwd)
apt="$cache/apt"
sysroot="$cache/sysroot"
mkdir -p "$apt/lists/partial" "$apt/cache/archives/partial" "$sysroot"
printf '%s\n' 'deb https://deb.debian.org/debian trixie main' \
    'deb https://deb.debian.org/debian-security trixie-security main' > "$apt/sources.list"
apt_command=(/usr/bin/apt-get -o "Dir::State::Lists=$apt/lists" -o "Dir::Cache=$apt/cache"
    -o "Dir::Etc::sourcelist=$apt/sources.list" -o Dir::Etc::sourceparts=-
    -o Debug::NoLocking=true -o "APT::Sandbox::User=$(id -un)" -o Acquire::Retries=2)
"${apt_command[@]}" update
"${apt_command[@]}" -y --download-only --no-install-recommends install \
    libboost-dev libfmt-dev libglib2.0-dev libgirepository1.0-dev libpango1.0-dev \
    libkf6coreaddons-dev libxcb1-dev libxkbcommon-dev libvulkan-dev libopengl-dev libgl-dev \
    libjpeg-dev liblz4-dev libxxhash-dev libssl-dev zlib1g-dev
for package in "$apt"/cache/archives/*.deb; do
    dpkg-deb -x "$package" "$sysroot"
done

# The extracted -dev packages also refer to runtimes the system already has.
mkdir -p "$sysroot/usr/lib/x86_64-linux-gnu"
for library in /usr/lib/x86_64-linux-gnu/*.so*; do
    destination="$sysroot/usr/lib/x86_64-linux-gnu/${library##*/}"
    if [[ ! -e "$destination" && ! -L "$destination" ]]; then
        ln -s "$library" "$destination"
    fi
done
if [[ -f "$sysroot/usr/include/x86_64-linux-gnu/jconfig.h" ]]; then
    ln -sfn x86_64-linux-gnu/jconfig.h "$sysroot/usr/include/jconfig.h"
fi
python_packages="$sysroot/usr/lib/python3/dist-packages"
if [[ -d "$python_packages/setuptools/_distutils" ]]; then
    ln -sfn setuptools/_distutils "$python_packages/distutils"
fi

uv venv --allow-existing --python /usr/bin/python3.13 "$cache/tools"
uv pip install --python "$cache/tools/bin/python" 'cmake>=3.25,<4' ninja aqtinstall
qt="$cache/qt/6.11.0/gcc_64"
if [[ ! -r "$qt/lib/libQt6Svg.so.6" || ! -r "$qt/lib/libicuuc.so.73" ]]; then
    (cd "$cache"
        "$cache/tools/bin/aqt" install-qt linux desktop 6.11.0 linux_gcc_64 \
            --archives qtbase qtsvg icu -O "$cache/qt")
fi
if [[ ! -r "$qt/plugins/imageformats/libqwebp.so" ]]; then
    (cd "$cache"
        "$cache/tools/bin/aqt" install-qt linux desktop 6.11.0 linux_gcc_64 \
            --noarchives -m qtimageformats -O "$cache/qt")
fi

{
    printf 'preview_deps=%q\n' "$cache"
    cat <<'ENV'
export OVYGRAM_PREVIEW_SYSROOT="$preview_deps/sysroot"
preview_qt="$preview_deps/qt/6.11.0/gcc_64"
preview_lib="$OVYGRAM_PREVIEW_SYSROOT/usr/lib/x86_64-linux-gnu"
export PATH="$preview_deps/tools/bin:$OVYGRAM_PREVIEW_SYSROOT/usr/bin:$PATH"
export LD_LIBRARY_PATH="$preview_qt/lib:$preview_lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export LIBRARY_PATH="$preview_lib${LIBRARY_PATH:+:$LIBRARY_PATH}"
export QT_PLUGIN_PATH="$preview_qt/plugins"
export CMAKE_PREFIX_PATH="$preview_qt:$OVYGRAM_PREVIEW_SYSROOT/usr${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"
export PKG_CONFIG_PATH="$preview_lib/pkgconfig:$OVYGRAM_PREVIEW_SYSROOT/usr/share/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export PKG_CONFIG_SYSROOT_DIR="$OVYGRAM_PREVIEW_SYSROOT"
export PYTHONPATH="$preview_lib/gobject-introspection:$OVYGRAM_PREVIEW_SYSROOT/usr/lib/python3/dist-packages${PYTHONPATH:+:$PYTHONPATH}"
export XDG_DATA_DIRS="$OVYGRAM_PREVIEW_SYSROOT/usr/share:${XDG_DATA_DIRS:-/usr/local/share:/usr/share}"
ENV
} > "$cache/environment.sh"
echo "Preview dependencies are ready in $cache"
