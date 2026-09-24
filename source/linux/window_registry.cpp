#include "window_registry.hpp"
#include <algorithm>

namespace reshade
{
	std::mutex s_windows_mutex;
	std::unordered_map<input::window_handle, registered_window> s_windows;

	size_t count_windows_on_display(const void *display)
	{
		const std::lock_guard<std::mutex> lock(s_windows_mutex);
		return std::count_if(s_windows.begin(), s_windows.end(),
			[display](const auto &entry) { return entry.second.display == display; });
	}
}
