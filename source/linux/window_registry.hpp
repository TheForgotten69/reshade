#pragma once

#include "input.hpp"
#include <mutex>
#include <unordered_map>
#include <unordered_set>

struct wl_display;

namespace reshade
{
	// Registered by the Vulkan WSI hooks before any input backend exists for a given window, so
	// that 'input::register_window' can later tell which backend the window needs and with what
	// initial swapchain extent. Shared between the Wayland/X11 backends and the platform-neutral
	// 'input' facade in 'input_linux.cpp', so it lives in its own translation unit rather than as
	// a file-local static of either.
	struct wayland_surface_info
	{
		wl_display *display = nullptr;
		unsigned int width = 0;
		unsigned int height = 0;
		std::unordered_set<uintptr_t> surfaces;
		std::shared_ptr<input> input_instance;
	};
	extern std::mutex s_wayland_surfaces_mutex;
	extern std::unordered_map<void *, wayland_surface_info> s_wayland_surfaces;

	struct x11_window_info
	{
		void *display = nullptr;
		input::x11_display_kind display_kind = input::x11_display_kind::xcb;
		unsigned int width = 0;
		unsigned int height = 0;
		std::unordered_set<uintptr_t> surfaces;
		std::shared_ptr<input> input_instance;
	};
	extern std::mutex s_x11_windows_mutex;
	extern std::unordered_map<void *, x11_window_info> s_x11_windows;
}
