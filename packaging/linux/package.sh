#!/usr/bin/env bash
set -euo pipefail

version="${1:?usage: package.sh VERSION [ARCH]}"
arch="${2:-$(uname -m)}"
project_dir="$(cd "$(dirname "$0")/../.." && pwd)"
name="fast-sigma-raw-${version}-linux-${arch}"
stage="$project_dir/dist/$name"

case "$stage" in
  "$project_dir"/dist/fast-sigma-raw-*) ;;
  *) echo "refusing unsafe staging path: $stage" >&2; exit 1 ;;
esac
rm -rf "$stage"
mkdir -p "$stage/licenses"

install -m 755 "$project_dir/target/release/fast-sigma-raw-gui" "$stage/"
install -m 755 "$project_dir/target/release/fast-sigma-raw" "$stage/"
install -m 644 "$project_dir/packaging/linux/fast-sigma-raw.desktop" "$stage/"
cp "$project_dir/README.md" "$project_dir/LICENSE" \
  "$project_dir/THIRD_PARTY_NOTICES.md" "$stage/"
cp "$project_dir/vendor/x3f-tools/LICENSE" "$stage/licenses/X3F_TOOLS_LICENSE.txt"
cp "$project_dir/packaging/licenses/LIBTIFF_LICENSE.md" "$stage/licenses/"
cp "$project_dir/packaging/licenses/X3FUSE_CORE_APACHE_LICENSE.txt" \
  "$stage/licenses/"

archive="$project_dir/dist/$name.tar.gz"
rm -f "$archive"
tar -C "$project_dir/dist" -czf "$archive" "$name"
echo "$archive"
