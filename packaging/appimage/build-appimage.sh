#!/usr/bin/env bash
# Assemble the Linux AppImage from a configured and built tree.
#
#   packaging/appimage/build-appimage.sh <build-dir> <output-dir>
#
# Needs linuxdeploy and its GTK plugin on PATH (the release workflow downloads
# pinned copies). wxGTK is a GTK3 toolkit, so the plugin bundles the GTK
# modules, schemas and pixbuf loaders the launcher needs.
set -euo pipefail

build_dir=${1:?usage: build-appimage.sh <build-dir> <output-dir>}
out_dir=${2:?usage: build-appimage.sh <build-dir> <output-dir>}
src_dir=$(cd "$(dirname "$0")/../.." && pwd)

version=$(head -n 1 "$src_dir/VERSION")
arch=$(uname -m)
appdir=$(mktemp -d)/AppDir
trap 'rm -rf "$(dirname "$appdir")"' EXIT

# A plain install leaves out the tar.gz's portable files, so the AppImage stores
# saves and settings in the user's data folder.
cmake --install "$build_dir" --prefix "$appdir/usr"

mkdir -p "$out_dir"
out_dir=$(cd "$out_dir" && pwd)
cd "$(dirname "$appdir")"

# The AppImage runs from a read-only mount; saves go to XDG_DATA_HOME and need
# no extra handling. linuxdeploy names the output after the desktop file, so
# rename it afterwards.
export DEPLOY_GTK_VERSION=3
export LINUXDEPLOY_OUTPUT_VERSION="$version"
export APPIMAGE_EXTRACT_AND_RUN=1
linuxdeploy \
    --appdir "$appdir" \
    --executable "$appdir/usr/bin/m2launcher" \
    --executable "$appdir/usr/bin/m2host" \
    --desktop-file "$appdir/usr/share/applications/imsa-wcr-m2-recomp.desktop" \
    --icon-file "$src_dir/packaging/icons/imsa-wcr-m2-recomp-256.png" \
    --icon-filename imsa-wcr-m2-recomp \
    --plugin gtk \
    --output appimage

mv ./*.AppImage "$out_dir/imsa-wcr-m2-recomp-$version-linux-$arch.AppImage"
