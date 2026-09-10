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

select_addons()
{
	if [ -n "${INSTALL_ADDONS+x}" ]; then
		# Scripted installs use stable add-on identifiers, not menu numbers.
		ADDON_SELECTION=$INSTALL_ADDONS
		return
	fi

	# A non-interactive installation deliberately selects no optional add-ons.
	if [ ! -t 0 ]; then
		ADDON_SELECTION=none
		return
	fi

	printf '%s\n' \
		'Optional native Linux add-ons are not installed by default.' \
		'Enter the numbers to install, separated by spaces or commas.' \
		'Enter "all" for every optional add-on, or press Enter for none.' \
		'  1) FPS Limit' \
		'  2) History Window' \
		'  3) API Trace' \
		'  4) Shader Dump' \
		'  5) Shader Replace' \
		'  6) Texture Dump' \
		'  7) Texture Replace' \
		'  8) Texture Overlay' \
		'  9) Effects During Frame' \
		' 10) Swapchain Override'
	printf 'Add-ons to install: '
	IFS= read -r ADDON_SELECTION || ADDON_SELECTION=
	ADDON_SELECTION=$(printf '%s' "$ADDON_SELECTION" | tr ',' ' ')
	[ -n "$ADDON_SELECTION" ] || ADDON_SELECTION=none
	ADDON_SELECTION_IS_MENU=1

	case $ADDON_SELECTION in
	all|none) return ;;
	esac
	for selected_number in $ADDON_SELECTION; do
		case $selected_number in
		1|2|3|4|5|6|7|8|9|10) ;;
		*) printf '%s\n' "Invalid add-on selection: $selected_number" >&2; exit 2 ;;
		esac
	done
}

want_addon()
{
	addon_id=$1
	menu_number=$2

	case $ADDON_SELECTION in
	all) return 0 ;;
	none) return 1 ;;
	esac

	if [ "${ADDON_SELECTION_IS_MENU:-0}" = 1 ]; then
		for selected_number in $ADDON_SELECTION; do
			[ "$selected_number" = "$menu_number" ] && return 0
		done
		return 1
	fi

	case ",$ADDON_SELECTION," in
	*,"$addon_id",*) return 0 ;;
	*) return 1 ;;
	esac
}

install_addon()
{
	addon_id=$1
	addon_label=$2
	menu_number=$3

	if want_addon "$addon_id" "$menu_number"; then
		install -m 755 "$PACKAGE_DIR/optional-addons/$addon_id.addon64" "$ADDON_DIR/$addon_id.addon64"
		printf '%s\n' "Installed optional add-on: $addon_label"
	fi
}

select_addons
install_addon fps_limit 'FPS Limit' 1
install_addon history_window 'History Window' 2
install_addon api_trace 'API Trace' 3
install_addon shader_dump 'Shader Dump' 4
install_addon shader_replace 'Shader Replace' 5
install_addon texture_dump 'Texture Dump' 6
install_addon texture_replace 'Texture Replace' 7
install_addon texture_overlay 'Texture Overlay' 8
install_addon effects_during_frame 'Effects During Frame' 9
install_addon swapchain_override 'Swapchain Override' 10

printf '%s\n' \
	"ReShade Linux Vulkan beta installed in $PREFIX." \
	'Launch a Vulkan application with: RESHADE_ENABLE=1 /path/to/application'
