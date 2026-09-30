// Linux implementation of 'reshade::input': picks the Wayland or X11 backend from the WSI kind the
// window was presented with, and forwards to it.
#include "input.hpp"
#include "dll_log.hpp"
#include "key_translation.hpp"
#include "wayland_input.hpp"
#include "window_registry.hpp"
#include "x11_input.hpp"
#include <algorithm>

namespace
{
	std::unique_ptr<reshade::input_backend> create_backend(reshade::input &owner, reshade::input::window_handle window, const reshade::registered_window &info)
	{
		if (info.kind == reshade::input::wsi_kind::wayland)
			return std::make_unique<reshade::wayland_input>(owner, static_cast<wl_display *>(info.display), static_cast<wl_surface *>(window));
		return std::make_unique<reshade::x11_input>(owner, static_cast<xcb_window_t>(reinterpret_cast<uintptr_t>(window)), info.display, info.kind);
	}
}

bool reshade::input::is_keyboard_layout_german()
{
	return s_keyboard_layout_german;
}

std::shared_ptr<reshade::input> reshade::input::register_window(window_handle window)
{
	// Must be called with 's_windows_mutex' held.
	const auto existing_input = [](const registered_window &registered) {
		if (registered.input_instance != nullptr)
			registered.input_instance->_backend->set_extent(registered.width, registered.height);
		return registered.input_instance;
	};

	registered_window info;
	{
		const std::lock_guard<std::mutex> lock(s_windows_mutex);
		const auto it = s_windows.find(window);
		if (it == s_windows.end())
			return nullptr;
		if (std::shared_ptr<input> existing = existing_input(it->second))
			return existing;
		info = it->second;
	}

	// Initialization may block on round trips, so it runs without holding the registry lock.
	auto result = std::make_shared<input>(window);
	std::unique_ptr<input_backend> backend = create_backend(*result, window, info);
	backend->set_extent(info.width, info.height);
	if (!backend->initialize())
	{
		log::message(log::level::warning, "Failed to initialize %s input for window %p.", backend->name(), window);
		return nullptr;
	}
	result->_backend = backend.release();

	const std::lock_guard<std::mutex> lock(s_windows_mutex);
	const auto it = s_windows.find(window);
	if (it == s_windows.end() || it->second.kind != info.kind || it->second.display != info.display)
		return nullptr;
	// Another runtime may have registered the same window meanwhile.
	if (std::shared_ptr<input> existing = existing_input(it->second))
		return existing;

	log::message(log::level::info, "Initialized %s input for window %p.", result->_backend->name(), window);
	it->second.input_instance = result;
	return result;
}

reshade::input::~input()
{
	delete _backend;
}

void reshade::input::register_surface(window_handle window, wsi_kind kind, void *display, uintptr_t vulkan_surface, unsigned int width, unsigned int height)
{
	const std::lock_guard<std::mutex> lock(s_windows_mutex);
	registered_window &info = s_windows[window];
	info.kind = kind;
	info.display = display;
	info.width = width;
	info.height = height;
	info.vulkan_surfaces.insert(vulkan_surface);
}

void reshade::input::unregister_surface(window_handle window, uintptr_t vulkan_surface)
{
	const std::lock_guard<std::mutex> lock(s_windows_mutex);
	const auto it = s_windows.find(window);
	if (it != s_windows.end() && it->second.vulkan_surfaces.erase(vulkan_surface) != 0 && it->second.vulkan_surfaces.empty())
		s_windows.erase(it);
}

const char *reshade::input::get_clipboard_text(void *user_data)
{
	// ImGui reads the returned string before calling again, from the single GUI thread.
	static std::string buffer;
	const auto *const self = static_cast<input *>(user_data);
	buffer = self != nullptr && self->_backend != nullptr ? self->_backend->clipboard_text() : std::string();
	return buffer.c_str();
}

void reshade::input::set_clipboard_text(void *user_data, const char *text)
{
	const auto *const self = static_cast<input *>(user_data);
	if (self != nullptr && self->_backend != nullptr)
		self->_backend->set_clipboard_text(text);
}

void reshade::input::register_window_with_raw_input(window_handle, bool, bool)
{
}

void reshade::input::next_frame()
{
	const std::unique_lock<std::recursive_mutex> lock(_mutex);

	std::copy(std::begin(_keys), std::end(_keys), std::begin(_last_keys));
	_key_transitions.clear();
	for (uint8_t &state : _keys)
		state &= 0x80;
	std::copy(std::begin(_mouse_position), std::end(_mouse_position), std::begin(_last_mouse_position));
	_mouse_wheel_delta = 0;
	_text_input.clear();
	++_frame_count;

	if (_backend == nullptr)
		return;
#if RESHADE_VERBOSE_LOG
	if (_frame_count == 1 || (_frame_count % 600) == 0)
		log::message(log::level::debug, "Input heartbeat frame=%llu backend=%s window=%p keyboard_focus=%d pointer_focus=%d.",
			static_cast<unsigned long long>(_frame_count), _backend->name(), _window, _backend->keyboard_focused(), _backend->pointer_focused());
#endif
	_backend->next_frame();
}

void reshade::input::max_mouse_position(unsigned int position[2]) const
{
	position[0] = _backend != nullptr ? _backend->width() : 1;
	position[1] = _backend != nullptr ? _backend->height() : 1;
}

bool reshade::input::is_mouse_position_valid() const
{
	return _backend != nullptr && _backend->pointer_focused();
}

bool reshade::input::needs_overlay_cursor() const
{
	return _backend != nullptr && _backend->needs_overlay_cursor();
}

void reshade::input::block_mouse_cursor_warping(bool enable)
{
	_block_cursor_warping = enable;
	if (_backend != nullptr)
		_backend->set_overlay_active(enable);
}

std::shared_ptr<reshade::input_gamepad> reshade::input_gamepad::load()
{
	return {};
}

void reshade::input_gamepad::next_frame()
{
}
