#!/usr/bin/env bash
set -euo pipefail

version="${1:?usage: package.sh VERSION [ARCH]}"
arch="${2:-$(uname -m)}"
project_dir="$(cd "$(dirname "$0")/../.." && pwd)"
name="fast-sigma-raw-${version}-macos-${arch}"
stage="$project_dir/dist/$name"
app="$stage/Fast Sigma Raw.app"

case "$stage" in
  "$project_dir"/dist/fast-sigma-raw-*) ;;
  *) echo "refusing unsafe staging path: $stage" >&2; exit 1 ;;
esac
rm -rf "$stage"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources/licenses"

install -m 755 "$project_dir/target/release/fast-sigma-raw-gui" \
  "$app/Contents/MacOS/fast-sigma-raw-gui"
install -m 755 "$project_dir/target/release/fast-sigma-raw" \
  "$stage/fast-sigma-raw"
sed "s/@VERSION@/$version/g" "$project_dir/packaging/macos/Info.plist.in" \
  > "$app/Contents/Info.plist"
plutil -lint "$app/Contents/Info.plist"

cp "$project_dir/README.md" "$project_dir/LICENSE" \
  "$project_dir/THIRD_PARTY_NOTICES.md" "$stage/"
cp "$project_dir/vendor/x3f-tools/LICENSE" \
  "$app/Contents/Resources/licenses/X3F_TOOLS_LICENSE.txt"
cp "$project_dir/packaging/licenses/LIBTIFF_LICENSE.md" \
  "$app/Contents/Resources/licenses/"
cp "$project_dir/packaging/licenses/X3FUSE_CORE_APACHE_LICENSE.txt" \
  "$app/Contents/Resources/licenses/"

# Ad-hoc signing records bundle integrity but does not identify a developer.
# GitHub releases remain unnotarized until maintainers configure Apple secrets.
codesign --force --deep --sign - "$app"

archive="$project_dir/dist/$name.zip"
rm -f "$archive"
ditto -c -k --sequesterRsrc --keepParent "$stage" "$archive"
echo "$archive"
