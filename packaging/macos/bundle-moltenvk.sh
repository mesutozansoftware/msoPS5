#!/usr/bin/env bash
# Downloads MoltenVK and installs it into an msoPS5 install directory: next to the flat
# kyty_emulator and inside msoPS5.app/Contents/Frameworks, ad-hoc signed.
#
# Usage: packaging/macos/bundle-moltenvk.sh <install-dir>
set -euo pipefail

MOLTENVK_VERSION="${MOLTENVK_VERSION:-v1.4.2}"
MOLTENVK_SHA256="${MOLTENVK_SHA256:-f95765a6229cb7b915990a2890ce12ebe36a730b021545d3d52ae69ce4c4024e}"

if [[ $# -ne 1 ]]; then
	echo "usage: $0 <install-dir>" >&2
	exit 2
fi

install_dir="$1"
app_dir="$install_dir/msoPS5.app"
test -d "$app_dir" || { echo "error: $app_dir not found" >&2; exit 1; }

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

archive="$work/MoltenVK-macos.tar"
curl --fail --location --retry 3 --output "$archive" \
	"https://github.com/KhronosGroup/MoltenVK/releases/download/$MOLTENVK_VERSION/MoltenVK-macos.tar"
echo "$MOLTENVK_SHA256  $archive" | shasum -a 256 --check
tar -xf "$archive" -C "$work"

package="$work/MoltenVK"
moltenvk="$install_dir/libMoltenVK.dylib"
install -m 755 "$package/MoltenVK/dynamic/dylib/macOS/libMoltenVK.dylib" "$moltenvk"
install -m 644 "$package/LICENSE" "$install_dir/LICENSE.MoltenVK"
codesign --force --sign - --timestamp=none "$moltenvk"

install -d -m 755 "$app_dir/Contents/Frameworks" "$app_dir/Contents/Resources"
install -m 755 "$moltenvk" "$app_dir/Contents/Frameworks/libMoltenVK.dylib"
install -m 644 "$package/LICENSE" "$app_dir/Contents/Resources/LICENSE.MoltenVK"
codesign --force --sign - --timestamp=none "$app_dir"
