#!/bin/sh
# Assemble the release archive from an installed build and a checkout of the standard shaders.
# Usage: package.sh <install prefix> <reshade-shaders checkout> <archive name> <output directory>
set -eu

STAGE=$1
SHADERS=$2
NAME=$3
OUTPUT=$4
SOURCE_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)

PACKAGE=$(mktemp -d)
trap 'rm -rf "$PACKAGE"' EXIT
ROOT="$PACKAGE/$NAME"

install -D -m 755 "$STAGE/lib/reshade/ReShade64.so" "$ROOT/lib/reshade/ReShade64.so"
install -D -m 644 "$STAGE/share/vulkan/implicit_layer.d/ReShade64.json" "$ROOT/share/vulkan/implicit_layer.d/ReShade64.json"

install -d "$ROOT/share/reshade/reshade-shaders"
cp -R "$SHADERS/Shaders" "$SHADERS/Textures" "$SHADERS/README.md" "$SHADERS/REFERENCE.md" "$ROOT/share/reshade/reshade-shaders/"

# Ship exactly the optional add-ons the installer offers
install -d "$ROOT/optional-addons"
for addon in $(sed -n 's/^install_addon \([a-z_]*\) .*/\1/p' "$SOURCE_DIR/tools/linux/install.sh"); do
	install -m 755 "$STAGE/share/reshade/$addon.addon64" "$ROOT/optional-addons/$addon.addon64"
done

install -m 755 "$SOURCE_DIR/tools/linux/install.sh" "$SOURCE_DIR/tools/linux/uninstall.sh" "$ROOT/"
install -m 644 "$SOURCE_DIR/tools/linux/README.md" "$SOURCE_DIR/LICENSE.md" "$ROOT/"

mkdir -p "$OUTPUT"
tar -C "$PACKAGE" -cJf "$OUTPUT/$NAME.tar.xz" "$NAME"
printf '%s\n' "$OUTPUT/$NAME.tar.xz"
