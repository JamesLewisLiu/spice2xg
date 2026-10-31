#!/usr/bin/env bash
set -euo pipefail

# Default to LAA for XG1 and keep an explicit alias for existing start scripts.
# Preserve the upstream LAA location and offer the standard binary separately.
if [[ $# -ne 3 ]]; then
    echo "Usage: package_launchers.sh BUILD_SPICETOOLS OUTPUT_DIRECTORY SOURCE_COMMIT" >&2
    exit 2
fi

build=$1
output=$2
revision=$3
for file in "$build/32/spice.exe" "$build/32/spice_laa.exe" "$build/spicecfg.exe"; do
    if [[ ! -f "$file" ]]; then
        echo "Missing launcher: $file" >&2
        exit 1
    fi
done

mkdir -p "$output/extras/largeaddressaware" "$output/extras/standard"
cp "$build/32/spice_laa.exe" "$output/spice.exe"
cp "$build/32/spice_laa.exe" "$output/spice_laa.exe"
cp "$build/32/spice_laa.exe" "$output/extras/largeaddressaware/spice.exe"
cp "$build/32/spice.exe" "$output/extras/standard/spice.exe"
cp "$build/spicecfg.exe" "$output/spicecfg.exe"

cat > "$output/BUILD_INFO.txt" <<EOF
spice2xg source commit: $revision

XG1 / XG1CN on 64-bit Windows:
The root spice.exe and spice_laa.exe are the same 32-bit LAA executable.
The identical LAA binary is also in extras/largeaddressaware/spice.exe.
The non-LAA launcher (2 GiB address space) is in extras/standard/spice.exe.
spice64.exe cannot load the original 32-bit XG1 game modules.

Replace the executable that your start script actually runs. Older copies
with other filenames are not updated automatically by extracting this ZIP.
Verify the launcher files against SHA256SUMS-launchers.txt before renaming.
EOF

(
    cd "$output"
    sha256sum spice.exe spice_laa.exe spicecfg.exe extras/largeaddressaware/spice.exe extras/standard/spice.exe \
        > SHA256SUMS-launchers.txt
)
