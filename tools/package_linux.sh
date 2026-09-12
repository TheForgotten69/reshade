#!/usr/bin/env bash
# Create a distributable native Linux/Vulkan ReShade archive from an existing build.
set -euo pipefail

usage() {
	printf '%s\n' "Usage: $0 <build-dir> <official-shaders-dir> [output-dir] [version]" >&2
	exit 2
}

[[ $# -ge 2 && $# -le 4 ]] || usage

build_dir=$1
shaders_dir=$2
output_dir=${3:-"$PWD"}
version=${4:-"$(git describe --tags --match 'v[0-9]*' --abbrev=0 2>/dev/null || true)"}
[[ -n "$version" ]] || { printf '%s\n' 'Unable to determine a version; pass it as the fourth argument.' >&2; exit 1; }

for command in cmake git tar; do
	command -v "$command" >/dev/null || { printf '%s\n' "Missing required command: $command" >&2; exit 1; }
done
[[ -d "$build_dir" ]] || { printf '%s\n' "Build directory does not exist: $build_dir" >&2; exit 1; }
git -C "$shaders_dir" rev-parse --is-inside-work-tree >/dev/null

package_name="reshade-linux-vulkan-${version}-x86_64"
staging_dir=$(mktemp -d)
trap 'rm -rf "$staging_dir"' EXIT
package_dir="$staging_dir/$package_name"

cmake --install "$build_dir" --prefix "$package_dir"
install -d "$package_dir/optional-addons" "$package_dir/share/reshade/reshade-shaders"

# Keep optional modules outside their load path. The release installer copies
# only the modules selected by the user into share/reshade.
addons=(
	fps_limit
	history_window
	api_trace
	shader_dump
	shader_replace
	texture_dump
	texture_replace
	texture_overlay
	effects_during_frame
	swapchain_override
)
for addon in "${addons[@]}"; do
	addon_path="$package_dir/share/reshade/$addon.addon64"
	[[ -f "$addon_path" ]] || { printf '%s\n' "Expected add-on was not built: $addon_path" >&2; exit 1; }
	mv "$addon_path" "$package_dir/optional-addons/"
done

# The FFmpeg-dependent video capture example is intentionally not distributed
# in this beta. It is not portable across applications that bundle FFmpeg.
rm -f "$package_dir/share/reshade"/*.addon "$package_dir/share/reshade"/*.addon64

# Archive only tracked files from the official shader checkout, never a
# caller's local third-party shaders or presets.
git -C "$shaders_dir" archive --format=tar HEAD | tar -xf - -C "$package_dir/share/reshade/reshade-shaders"

install -m 644 LICENSE.md "$package_dir/LICENSE.md"
install -m 644 tools/linux/README.md "$package_dir/README.md"
install -m 755 tools/linux/install.sh "$package_dir/install.sh"
install -m 755 tools/linux/uninstall.sh "$package_dir/uninstall.sh"

install -d "$output_dir"
archive="$output_dir/$package_name.tar.xz"
tar --sort=name --owner=0 --group=0 --numeric-owner -C "$staging_dir" -cJf "$archive" "$package_name"
printf '%s\n' "Created $archive"
