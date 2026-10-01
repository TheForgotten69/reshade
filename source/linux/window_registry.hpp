#pragma once

#include "input.hpp"
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace reshade
{
	// Native windows seen by the Vulkan WSI hooks, keyed by their 'input::window_handle'.
	struct registered_window
	{
		input::wsi_kind kind = input::wsi_kind::wayland;
		void *display = nullptr;
		unsigned int width = 0;
		unsigned int height = 0;
		std::unordered_set<uintptr_t> vulkan_surfaces;
		std::shared_ptr<input> input_instance;
	};

	extern std::mutex s_windows_mutex;
	extern std::unordered_map<input::window_handle, registered_window> s_windows;

	// Number of registered windows presented through 'display'. Acquires 's_windows_mutex'.
	size_t count_windows_on_display(const void *display);
}
