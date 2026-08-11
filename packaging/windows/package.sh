#!/usr/bin/env bash
set -euo pipefail

version="${1:?usage: package.sh VERSION [ARCH]}"
arch="${2:-x86_64}"
project_dir="$(cd "$(dirname "$0")/../.." && pwd)"
target_dir="$project_dir/target/x86_64-pc-windows-gnu/release"
name="fast-sigma-raw-${version}-windows-${arch}"
stage="$project_dir/dist/$name"

case "$stage" in
  "$project_dir"/dist/fast-sigma-raw-*) ;;
  *) echo "refusing unsafe staging path: $stage" >&2; exit 1 ;;
esac
rm -rf "$stage"
mkdir -p "$stage/licenses"

cp "$target_dir/fast-sigma-raw-gui.exe" "$target_dir/fast-sigma-raw.exe" "$stage/"

# Copy only non-system UCRT64 DLL dependencies. `ldd` reports the transitive
# closure, so libtiff's codec dependencies are included as well.
for executable in "$stage/fast-sigma-raw-gui.exe" "$stage/fast-sigma-raw.exe"; do
  while IFS= read -r library; do
    if [[ "$library" =~ ^[A-Za-z]:\\ ]]; then
      library="$(cygpath -u "$library")"
    fi
    if [[ -f "$library" && "$library" == /ucrt64/bin/* ]]; then
      cp -n "$library" "$stage/"
    fi
  done < <(ldd "$executable" | awk '/=>/ { print $3 }')
done

cp "$project_dir/README.md" "$project_dir/LICENSE" \
  "$project_dir/THIRD_PARTY_NOTICES.md" "$stage/"
cp "$project_dir/vendor/x3f-tools/LICENSE" "$stage/licenses/X3F_TOOLS_LICENSE.txt"
cp "$project_dir/packaging/licenses/LIBTIFF_LICENSE.md" "$stage/licenses/"

archive="$project_dir/dist/$name.zip"
rm -f "$archive"
(cd "$project_dir/dist" && 7z a -tzip "$archive" "$name" >/dev/null)
echo "$archive"
