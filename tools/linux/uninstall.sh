#!/bin/sh
# Remove the native Linux/Vulkan ReShade host while preserving user data.
set -eu

PREFIX=${PREFIX:-"$HOME/.local"}

rm -f \
	"$PREFIX/lib/reshade/ReShade64.so" \
	"$PREFIX/share/vulkan/implicit_layer.d/ReShade64.json"

printf '%s\n' 'ReShade Linux Vulkan host removed.'
printf '%s\n' 'Shaders, optional add-ons, application configurations, presets and logs were preserved.'

