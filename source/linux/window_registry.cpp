#include "window_registry.hpp"

namespace reshade
{
	std::mutex s_wayland_surfaces_mutex;
	std::unordered_map<void *, wayland_surface_info> s_wayland_surfaces;

	std::mutex s_x11_windows_mutex;
	std::unordered_map<void *, x11_window_info> s_x11_windows;
}
