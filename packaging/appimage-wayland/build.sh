#!/bin/bash
# Builds Flameshot for Wayland (unofficial) as an AppImage.
# Run as root in a clean Ubuntu 24.04 container. The GitHub workflow
# .github/workflows/wayland-appimage.yml does this; to build it yourself:
#   docker run --rm -v "$PWD:/src" -w /src ubuntu:24.04 packaging/appimage-wayland/build.sh
# The AppImage ends up in the source folder.
set -euo pipefail

SRC=$(cd "$(dirname "$0")/../.." && pwd)
VERSION=${VERSION:-$(sed -n 's/.*set(FLAMESHOT_VERSION \([0-9.]*\)).*/\1/p' "$SRC/CMakeLists.txt")-wayland}
WORK=${WORK:-/tmp/appimage-build}

export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends \
  build-essential cmake extra-cmake-modules git ca-certificates wget file \
  qt6-base-dev qt6-tools-dev qt6-tools-dev-tools qt6-l10n-tools libqt6svg6-dev \
  qt6-image-formats-plugins qt6-wayland qt6-wayland-dev \
  libgl-dev appstream hicolor-icon-theme zsync

mkdir -p "$WORK"
cd "$WORK"
for tool in linuxdeploy linuxdeploy-plugin-qt linuxdeploy-plugin-appimage; do
  [ -x "$tool-x86_64.AppImage" ] ||
    wget -q "https://github.com/linuxdeploy/$tool/releases/download/continuous/$tool-x86_64.AppImage"
  chmod +x "$tool-x86_64.AppImage"
  # Clear the AppImage marker bytes, so a host with an AppImage handler
  # registered in binfmt_misc (such as AppImageLauncher) doesn't take over
  # running the tools inside the container
  printf '\0\0\0' | dd of="$tool-x86_64.AppImage" bs=1 seek=8 conv=notrunc status=none
done
# Containers have no FUSE, so the tools unpack themselves instead of mounting
export APPIMAGE_EXTRACT_AND_RUN=1

# The update checker and Imgur upload are off: the checker would point to the
# official releases, and Imgur no longer accepts the shared Flameshot key
cmake -S "$SRC" -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
  -DUSE_LAUNCHER_ABSOLUTE_PATH=OFF -DDISABLE_UPDATE_CHECKER=ON -DENABLE_IMGUR=OFF
cmake --build build -j"$(nproc)"
rm -rf AppDir
DESTDIR="$WORK/AppDir" cmake --install build
rm -rf AppDir/usr/include AppDir/usr/lib

export QMAKE=/usr/lib/qt6/bin/qmake
export EXTRA_PLATFORM_PLUGINS="libqwayland-generic.so"
export EXTRA_QT_MODULES="waylandcompositor"
export LDAI_OUTPUT="Flameshot-$VERSION-x86_64.AppImage"
export LDAI_UPDATE_INFORMATION="gh-releases-zsync|SamuelCGilbert|flameshot|latest|Flameshot-*-x86_64.AppImage.zsync"
./linuxdeploy-x86_64.AppImage --appdir AppDir --plugin qt --output appimage

mv "$LDAI_OUTPUT" "$LDAI_OUTPUT.zsync" "$SRC/"
cd "$SRC"
sha256sum "$LDAI_OUTPUT" > "$LDAI_OUTPUT.sha256sum"
cat "$LDAI_OUTPUT.sha256sum"
