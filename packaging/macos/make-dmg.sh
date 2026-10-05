#!/usr/bin/env bash
# Packs msoPS5.app into a compressed drag-to-install disk image with an Applications link.
#
# Usage: packaging/macos/make-dmg.sh <path/to/msoPS5.app> <output.dmg>
set -euo pipefail

if [[ $# -ne 2 ]]; then
	echo "usage: $0 <path/to/msoPS5.app> <output.dmg>" >&2
	exit 2
fi

app_dir="$1"
output="$2"
test -d "$app_dir/Contents/MacOS" || { echo "error: $app_dir is not an app bundle" >&2; exit 1; }

staging="$(mktemp -d)"
trap 'rm -rf "$staging"' EXIT

# ditto keeps the bundle's symlinks, permissions and code signatures intact.
ditto "$app_dir" "$staging/msoPS5.app"
ln -s /Applications "$staging/Applications"

rm -f "$output"
# hdiutil occasionally fails with "Resource busy" on CI machines; retry a few times.
for attempt in 1 2 3; do
	if hdiutil create -volname "msoPS5" -srcfolder "$staging" -fs HFS+ \
		-format UDZO -imagekey zlib-level=9 -ov "$output"; then
		break
	fi
	if [[ $attempt -eq 3 ]]; then
		echo "error: hdiutil create failed" >&2
		exit 1
	fi
	sleep $((attempt * 5))
done

hdiutil verify "$output"
