// ReShade-facing facade over the Linux input backends: selects Wayland or X11/XCB per window,
// owns the registries the Vulkan WSI hooks populate before a backend exists, and forwards the
// generic 'reshade::input' interface to whichever backend a given window uses. Backend-specific
// protocol handling lives in 'wayland_input.hpp' and 'x11_input.hpp'.
#include "input.hpp"
#include "dll_log.hpp"
#include "key_translation.hpp"
#include "window_registry.hpp"
#include "wayland_input.hpp"
#include "x11_input.hpp"
#include <algorithm>
#include <memory>
#include <mutex>

bool reshade::input::is_keyboard_layout_german()
{
	return s_keyboard_layout_german;
}
std::shared_ptr<reshade::input> reshade::input::register_window(window_handle window)
{
	wayland_surface_info surface_info;
	bool is_wayland_surface = false;
	{
		std::lock_guard<std::mutex> lock(s_wayland_surfaces_mutex);
		const auto it = s_wayland_surfaces.find(window);
		if (it != s_wayland_surfaces.end() && it->second.display != nullptr)
		{
			is_wayland_surface = true;
			if (const std::shared_ptr<input> existing = it->second.input_instance)
			{
#if RESHADE_VERBOSE_LOG
				log::message(log::level::debug, "Reusing Wayland input surface=%p frame=%llu keyboard_focus=%d pointer_focus=%d.", window, static_cast<unsigned long long>(existing->_frame_count), existing->_wayland->keyboard_focused, existing->_wayland->pointer_focused);
#endif
				existing->_wayland->width = std::max(1u, it->second.width);
				existing->_wayland->height = std::max(1u, it->second.height);
				return existing;
			}
			surface_info = it->second;
		}
	}

	if (is_wayland_surface)
	{
		// Wayland round trips may block. Do not hold the surface registry lock while creating
		// the input context, so swapchain destruction and concurrent surface updates can proceed.
		auto result = std::make_shared<input>(window);
		result->_wayland = new wayland_input_context{result.get(), surface_info.display, static_cast<wl_surface *>(window)};
		result->_wayland->width = std::max(1u, surface_info.width);
		result->_wayland->height = std::max(1u, surface_info.height);
		if (!result->_wayland->initialize())
		{
			log::message(log::level::warning, "Failed to initialize Wayland input for surface %p.", window);
			delete result->_wayland;
			result->_wayland = nullptr;
			return nullptr;
		}

		std::lock_guard<std::mutex> lock(s_wayland_surfaces_mutex);
		const auto it = s_wayland_surfaces.find(window);
		if (it == s_wayland_surfaces.end() || it->second.display != surface_info.display)
			return nullptr;
		if (const std::shared_ptr<input> existing = it->second.input_instance)
		{
#if RESHADE_VERBOSE_LOG
			log::message(log::level::debug, "Reusing Wayland input after initialization race surface=%p frame=%llu.", window, static_cast<unsigned long long>(existing->_frame_count));
#endif
			existing->_wayland->width = std::max(1u, it->second.width);
			existing->_wayland->height = std::max(1u, it->second.height);
			return existing;
		}

		log::message(log::level::info, "Initialized Wayland input for surface %p.", window);
		it->second.input_instance = result;
		return result;
	}

	x11_window_info x11_info;
	{
		std::lock_guard<std::mutex> lock(s_x11_windows_mutex);
		const auto it = s_x11_windows.find(window);
		if (it == s_x11_windows.end())
			return nullptr;
		if (const std::shared_ptr<input> existing = it->second.input_instance)
		{
#if RESHADE_VERBOSE_LOG
			log::message(log::level::debug, "Reusing X11 input window=%p frame=%llu keyboard_focus=%d pointer_focus=%d.", window, static_cast<unsigned long long>(existing->_frame_count), existing->_x11->keyboard_focused, existing->_x11->pointer_focused);
#endif
			existing->_x11->width = std::max(1u, it->second.width);
			existing->_x11->height = std::max(1u, it->second.height);
			return existing;
		}
		x11_info = it->second;
	}

	auto result = std::make_shared<input>(window);
	result->_x11 = new x11_input_context{result.get(), static_cast<xcb_window_t>(reinterpret_cast<uintptr_t>(window)), x11_info.display, x11_info.display_kind};
	result->_x11->width = std::max(1u, x11_info.width);
	result->_x11->height = std::max(1u, x11_info.height);
	if (!result->_x11->initialize())
	{
		log::message(log::level::warning, "Failed to initialize X11 input for window %p.", window);
		delete result->_x11;
		result->_x11 = nullptr;
		return nullptr;
	}
	std::lock_guard<std::mutex> lock(s_x11_windows_mutex);
	const auto it = s_x11_windows.find(window);
	if (it == s_x11_windows.end())
		return nullptr;
	if (const std::shared_ptr<input> existing = it->second.input_instance)
	{
#if RESHADE_VERBOSE_LOG
		log::message(log::level::debug, "Reusing X11 input after initialization race window=%p frame=%llu.", window, static_cast<unsigned long long>(existing->_frame_count));
#endif
		return existing;
	}
	log::message(log::level::info, "Initialized X11 input for window %p.", window);
	it->second.input_instance = result;
	return result;
}
reshade::input::~input()
{
	delete _wayland;
	delete _x11;
}
void reshade::input::register_wayland_surface(window_handle surface, void *display, uintptr_t vulkan_surface, unsigned int width, unsigned int height)
{
	std::lock_guard<std::mutex> lock(s_wayland_surfaces_mutex);
	auto [it, inserted] = s_wayland_surfaces.try_emplace(surface);
	it->second.display = static_cast<wl_display *>(display);
	it->second.width = width;
	it->second.height = height;
	it->second.surfaces.insert(vulkan_surface);
}
void reshade::input::unregister_wayland_surface(window_handle surface, uintptr_t vulkan_surface)
{
	std::lock_guard<std::mutex> lock(s_wayland_surfaces_mutex);
	const auto it = s_wayland_surfaces.find(surface);
	if (it != s_wayland_surfaces.end() && it->second.surfaces.erase(vulkan_surface) != 0 && it->second.surfaces.empty())
		s_wayland_surfaces.erase(it);
}
void reshade::input::register_x11_window(window_handle window, void *display, x11_display_kind display_kind, uintptr_t vulkan_surface, unsigned int width, unsigned int height)
{
	std::lock_guard<std::mutex> lock(s_x11_windows_mutex);
	auto [it, inserted] = s_x11_windows.try_emplace(window);
	it->second.display = display;
	it->second.display_kind = display_kind;
	it->second.width = width;
	it->second.height = height;
	it->second.surfaces.insert(vulkan_surface);
}
void reshade::input::unregister_x11_window(window_handle window, uintptr_t vulkan_surface)
{
	std::lock_guard<std::mutex> lock(s_x11_windows_mutex);
	const auto it = s_x11_windows.find(window);
	if (it != s_x11_windows.end() && it->second.surfaces.erase(vulkan_surface) != 0 && it->second.surfaces.empty())
		s_x11_windows.erase(it);
}
const char *reshade::input::get_clipboard_text(void *user_data)
{
	// Owns the string so the returned pointer stays valid for ImGui to read; only ever called
	// from the single-threaded GUI update, so a function-local static is safe here.
	static std::string buffer;
	auto *self = static_cast<input *>(user_data);
	buffer = self != nullptr && self->_wayland != nullptr ? self->_wayland->get_clipboard_text() : std::string();
	return buffer.c_str();
}
void reshade::input::set_clipboard_text(void *user_data, const char *text)
{
	auto *self = static_cast<input *>(user_data);
	if (self != nullptr && self->_wayland != nullptr)
		self->_wayland->set_clipboard_text(text);
}
void reshade::input::register_window_with_raw_input(window_handle, bool, bool) {}
void reshade::input::next_frame()
{
	const std::unique_lock<std::recursive_mutex> lock(_mutex);
	std::copy(std::begin(_keys), std::end(_keys), std::begin(_last_keys));
	for (uint8_t &state : _keys)
		state &= ~0x08;
	std::copy(std::begin(_mouse_position), std::end(_mouse_position), std::begin(_last_mouse_position));
	_mouse_wheel_delta = 0;
	_text_input.clear();
	++_frame_count;
#if RESHADE_VERBOSE_LOG
	// Periodic, throttled state dump to make focus/backend desync bugs reproducible from a log
	// alone, without needing to attach a debugger to the game process.
	if (_frame_count == 1 || (_frame_count % 600) == 0)
	{
		if (_wayland != nullptr)
			log::message(log::level::debug, "Input heartbeat frame=%llu backend=Wayland surface=%p keyboard_focus=%d pointer_focus=%d.", static_cast<unsigned long long>(_frame_count), _wayland->surface, _wayland->keyboard_focused, _wayland->pointer_focused);
		else if (_x11 != nullptr)
			log::message(log::level::debug, "Input heartbeat frame=%llu backend=X11 window=%#x keyboard_focus=%d pointer_focus=%d.", static_cast<unsigned long long>(_frame_count), _x11->window, _x11->keyboard_focused, _x11->pointer_focused);
		else
			log::message(log::level::debug, "Input heartbeat frame=%llu backend=none.", static_cast<unsigned long long>(_frame_count));
	}
#endif
	if (_wayland != nullptr && !_wayland->dispatch_pending_events())
		log::message(log::level::warning, "Wayland input pending-event dispatch failed for surface %p.", _wayland->surface);
	if (_x11 != nullptr)
		_x11->next_frame();
}
void reshade::input::max_mouse_position(unsigned int position[2]) const
{
	if (_wayland != nullptr)
	{
		_wayland->max_pointer_position(position);
		return;
	}
	if (_x11 != nullptr)
	{
		position[0] = _x11->width;
		position[1] = _x11->height;
		return;
	}
	position[0] = position[1] = 1;
}
void reshade::input::block_mouse_cursor_warping(bool enable)
{
	_block_cursor_warping = enable;
	if (_wayland != nullptr)
		_wayland->set_software_cursor_active(enable);
	if (_x11 != nullptr)
		_x11->set_native_cursor_hidden(enable);
}
std::shared_ptr<reshade::input_gamepad> reshade::input_gamepad::load()
{
	return {};
}
void reshade::input_gamepad::next_frame() {}
