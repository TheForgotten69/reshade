#!/bin/sh
# Install the native Linux/Vulkan ReShade beta from a release archive.
set -eu

PACKAGE_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PREFIX=${PREFIX:-"$HOME/.local"}
ADDON_DIR="$PREFIX/share/reshade"

install -d \
	"$PREFIX/lib/reshade" \
	"$PREFIX/share/vulkan/implicit_layer.d" \
	"$ADDON_DIR" \
	"$ADDON_DIR/reshade-shaders"

install -m 755 "$PACKAGE_DIR/lib/reshade/ReShade64.so" "$PREFIX/lib/reshade/ReShade64.so"
install -m 644 "$PACKAGE_DIR/share/vulkan/implicit_layer.d/ReShade64.json" "$PREFIX/share/vulkan/implicit_layer.d/ReShade64.json"

# Merge the standard shader collection. Do not delete an existing collection:
# it may contain presets or shader packs installed by the user.
cp -R "$PACKAGE_DIR/share/reshade/reshade-shaders/." "$ADDON_DIR/reshade-shaders/"

want_addon()
{
	addon_id=$1
	addon_label=$2

	case ",${INSTALL_ADDONS:-}," in
	*,all,*|*,"$addon_id",*) return 0 ;;
	*,none,*) return 1 ;;
	esac

	# A non-interactive installation deliberately selects no optional add-ons.
	if [ ! -t 0 ]; then
		return 1
	fi

	printf 'Install %s? [y/N] ' "$addon_label"
	IFS= read -r answer || answer=n
	case $answer in
	y|Y|yes|YES|Yes) return 0 ;;
	*) return 1 ;;
	esac
}

install_addon()
{
	addon_id=$1
	addon_label=$2

	if want_addon "$addon_id" "$addon_label"; then
		install -m 755 "$PACKAGE_DIR/optional-addons/$addon_id.addon64" "$ADDON_DIR/$addon_id.addon64"
		printf '%s\n' "Installed optional add-on: $addon_label"
	fi
}

printf '%s\n' 'Optional native Linux add-ons are disabled by default.'
printf '%s\n' 'Select only the tools you intend to use; installed add-ons load automatically.'
install_addon fps_limit 'FPS Limit'
install_addon history_window 'History Window'
install_addon api_trace 'API Trace'
install_addon shader_dump 'Shader Dump'
install_addon shader_replace 'Shader Replace'
install_addon texture_dump 'Texture Dump'
install_addon texture_replace 'Texture Replace'
install_addon texture_overlay 'Texture Overlay'
install_addon effects_during_frame 'Effects During Frame'
install_addon swapchain_override 'Swapchain Override'

printf '%s\n' \
	"ReShade Linux Vulkan beta installed in $PREFIX." \
	'Launch a Vulkan application with: RESHADE_ENABLE=1 /path/to/application'

