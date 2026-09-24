#include "wayland_input.hpp"
#include "dll_log.hpp"
#include "key_translation.hpp"
#include "window_registry.hpp"
#include "wine_input_bridge.hpp"
#include "relative-pointer-unstable-v1-client-protocol.h"
#include <algorithm>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

namespace
{
	template <typename T>
	void set_queue(T *proxy, wl_event_queue *queue)
	{
		wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(proxy), queue);
	}

	auto self(void *data) { return static_cast<reshade::wayland_input *>(data); }

	// Only a release request makes the compositor stop sending events to these objects.
	void release_pointer_proxy(wl_pointer *pointer)
	{
		if (pointer == nullptr)
			return;
		if (wl_pointer_get_version(pointer) >= WL_POINTER_RELEASE_SINCE_VERSION)
			wl_pointer_release(pointer);
		else
			wl_pointer_destroy(pointer);
	}
	void release_keyboard_proxy(wl_keyboard *keyboard)
	{
		if (keyboard == nullptr)
			return;
		if (wl_keyboard_get_version(keyboard) >= WL_KEYBOARD_RELEASE_SINCE_VERSION)
			wl_keyboard_release(keyboard);
		else
			wl_keyboard_destroy(keyboard);
	}

	const wl_registry_listener registry_listener = {
		[](void *data, wl_registry *registry, uint32_t name, const char *interface, uint32_t version) { self(data)->on_global(registry, name, interface, version); },
		[](void *data, wl_registry *, uint32_t name) { self(data)->on_global_remove(name); },
	};
	const wl_seat_listener seat_listener = {
		[](void *data, wl_seat *, uint32_t capabilities) { self(data)->on_seat_capabilities(capabilities); },
		[](void *, wl_seat *, const char *) {},
	};
	const wl_keyboard_listener keyboard_listener = {
		[](void *data, wl_keyboard *, uint32_t format, int fd, uint32_t size) { self(data)->on_keymap(format, fd, size); },
		[](void *data, wl_keyboard *, uint32_t, wl_surface *surface, wl_array *) { self(data)->on_keyboard_enter(surface); },
		[](void *data, wl_keyboard *, uint32_t, wl_surface *) { self(data)->on_keyboard_leave(); },
		[](void *data, wl_keyboard *, uint32_t serial, uint32_t, uint32_t key, uint32_t state) { self(data)->on_key(serial, key, state); },
		[](void *data, wl_keyboard *, uint32_t, uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group) { self(data)->on_modifiers(depressed, latched, locked, group); },
		[](void *, wl_keyboard *, int32_t, int32_t) {},
	};
	const wl_pointer_listener pointer_listener = {
		[](void *data, wl_pointer *, uint32_t serial, wl_surface *surface, wl_fixed_t x, wl_fixed_t y) { self(data)->on_pointer_enter(serial, surface, wl_fixed_to_double(x), wl_fixed_to_double(y)); },
		[](void *data, wl_pointer *, uint32_t, wl_surface *) { self(data)->on_pointer_leave(); },
		[](void *data, wl_pointer *, uint32_t, wl_fixed_t x, wl_fixed_t y) { self(data)->on_pointer_motion(wl_fixed_to_double(x), wl_fixed_to_double(y)); },
		[](void *data, wl_pointer *, uint32_t serial, uint32_t, uint32_t button, uint32_t state) { self(data)->on_pointer_button(serial, button, state); },
		[](void *data, wl_pointer *pointer, uint32_t, uint32_t axis, wl_fixed_t value) {
			self(data)->on_pointer_axis(axis, wl_fixed_to_double(value));
			// Before version 5 there are no frame events, so every axis event is a frame of its own.
			if (wl_pointer_get_version(pointer) < WL_POINTER_FRAME_SINCE_VERSION)
				self(data)->on_pointer_frame();
		},
		[](void *data, wl_pointer *) { self(data)->on_pointer_frame(); },
		[](void *, wl_pointer *, uint32_t) {},
		[](void *, wl_pointer *, uint32_t, uint32_t) {},
		[](void *data, wl_pointer *, uint32_t axis, int32_t steps) { self(data)->on_pointer_axis_discrete(axis, steps); },
	};
	const zwp_relative_pointer_v1_listener relative_pointer_listener = {
		[](void *data, zwp_relative_pointer_v1 *, uint32_t utime_hi, uint32_t utime_lo, wl_fixed_t dx, wl_fixed_t dy, wl_fixed_t, wl_fixed_t) {
			self(data)->on_relative_motion(wl_fixed_to_double(dx), wl_fixed_to_double(dy), (static_cast<uint64_t>(utime_hi) << 32) | utime_lo);
		},
	};
}

reshade::wayland_input::wayland_input(input &owner, wl_display *display, wl_surface *surface) :
	input_backend(owner),
	_display(display),
	_surface(surface),
	_xkb_context(xkb_context_new(XKB_CONTEXT_NO_FLAGS))
{
}

reshade::wayland_input::~wayland_input()
{
	_wine.release_cursor_clip(false);
	_overlay.reset();
	if (_relative_pointer != nullptr)
		zwp_relative_pointer_v1_destroy(_relative_pointer);
	if (_relative_pointer_manager != nullptr)
		zwp_relative_pointer_manager_v1_destroy(_relative_pointer_manager);
	release_pointer_proxy(_pointer_device);
	release_keyboard_proxy(_keyboard);
	if (_seat != nullptr)
	{
		if (wl_seat_get_version(_seat) >= WL_SEAT_RELEASE_SINCE_VERSION)
			wl_seat_release(_seat);
		else
			wl_seat_destroy(_seat);
	}
	if (_registry != nullptr)
		wl_registry_destroy(_registry);
	if (_queue != nullptr)
		wl_event_queue_destroy(_queue);

	xkb_state_unref(_xkb_state);
	xkb_keymap_unref(_keymap);
	xkb_context_unref(_xkb_context);
}

bool reshade::wayland_input::initialize()
{
	_wine_host = wine_input_bridge::is_wine_process();
	if (_wine_host)
		_wine.initialize();
	_queue = wl_display_create_queue(_display);
	if (_queue == nullptr || _xkb_context == nullptr)
		return false;

	// Create the registry directly on the private queue. Moving it there after creation races
	// the host's own dispatching of the default queue.
	auto *const display_wrapper = static_cast<wl_display *>(wl_proxy_create_wrapper(_display));
	if (display_wrapper == nullptr)
		return false;
	set_queue(display_wrapper, _queue);
	_registry = wl_display_get_registry(display_wrapper);
	wl_proxy_wrapper_destroy(display_wrapper);
	if (_registry == nullptr)
		return false;
	wl_registry_add_listener(_registry, &registry_listener, this);

	// The first round trip binds the globals, the second receives the seat capabilities (creating
	// the devices), the third their initial state such as the keymap and current focus.
	for (int i = 0; i < 3; ++i)
	{
		if (wl_display_roundtrip_queue(_display, _queue) < 0 || _seat == nullptr)
			return false;
		if (i == 0)
		{
			_overlay.attach(_surface);
		}
	}

	_clipboard = wayland_clipboard::get(_display);
	_pointer.set_extent(width(), height());
	update_pointer_scale();
	publish_pointer();

	log::message(log::level::info, "Wayland input: wl_display=%p vulkan_surface=%p keyboard=%s pointer=%s xkb_state=%s relative_pointer=%s.",
		_display, _surface, _keyboard != nullptr ? "yes" : "no", _pointer_device != nullptr ? "yes" : "no", _xkb_state != nullptr ? "yes" : "no", _relative_pointer != nullptr ? "yes" : "no");
	return true;
}

void reshade::wayland_input::next_frame()
{
	// The host is the only reader of its connection. Reading the socket here could block behind
	// a host thread that prepared a read, so only events already routed to the queue are handled.
	_pointer.set_extent(width(), height());
	const int result = wl_display_dispatch_queue_pending(_display, _queue);
	if (_clipboard != nullptr)
		_clipboard->dispatch();
	update_pointer_scale();
	publish_pointer();
	update_capture();
	_wine.release_cursor_clip(overlay_active());

	if (result < 0)
		log::message(log::level::warning, "Wayland input pending-event dispatch failed for surface %p.", _surface);
}

void reshade::wayland_input::on_global(wl_registry *registry, uint32_t name, const char *interface, uint32_t version)
{
	_overlay.bind(registry, name, interface);

	if (std::strcmp(interface, wl_seat_interface.name) == 0 && _seat == nullptr)
	{
		_seat = static_cast<wl_seat *>(wl_registry_bind(registry, name, &wl_seat_interface, std::min(version, 5u)));
		_seat_name = name;
		set_queue(_seat, _queue);
		wl_seat_add_listener(_seat, &seat_listener, this);
	}
	else if (std::strcmp(interface, zwp_relative_pointer_manager_v1_interface.name) == 0 && _relative_pointer_manager == nullptr)
	{
		_relative_pointer_manager = static_cast<zwp_relative_pointer_manager_v1 *>(wl_registry_bind(registry, name, &zwp_relative_pointer_manager_v1_interface, 1));
		set_queue(_relative_pointer_manager, _queue);
		bind_relative_pointer();
	}
}

void reshade::wayland_input::on_global_remove(uint32_t name)
{
	if (name != _seat_name || _seat == nullptr)
		return;

	release_keyboard_device();
	release_pointer_device();
	wl_seat_destroy(_seat);
	_seat = nullptr;
	_seat_name = 0;
}

void reshade::wayland_input::on_seat_capabilities(uint32_t capabilities)
{
	const bool has_keyboard = (capabilities & WL_SEAT_CAPABILITY_KEYBOARD) != 0;
	if (has_keyboard && _keyboard == nullptr)
	{
		_keyboard = wl_seat_get_keyboard(_seat);
		set_queue(_keyboard, _queue);
		wl_keyboard_add_listener(_keyboard, &keyboard_listener, this);
	}
	else if (!has_keyboard)
	{
		release_keyboard_device();
	}

	const bool has_pointer = (capabilities & WL_SEAT_CAPABILITY_POINTER) != 0;
	if (has_pointer && _pointer_device == nullptr)
	{
		_pointer_device = wl_seat_get_pointer(_seat);
		set_queue(_pointer_device, _queue);
		wl_pointer_add_listener(_pointer_device, &pointer_listener, this);
		bind_relative_pointer();
	}
	else if (!has_pointer)
	{
		release_pointer_device();
	}
}

void reshade::wayland_input::on_keymap(uint32_t format, int fd, uint32_t size)
{
	if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 || size == 0)
	{
		close(fd);
		return;
	}

	void *const data = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (data == MAP_FAILED)
		return;
	xkb_keymap *const keymap = xkb_keymap_new_from_string(_xkb_context, static_cast<const char *>(data), XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
	munmap(data, size);
	if (keymap == nullptr)
		return;

	xkb_state_unref(_xkb_state);
	xkb_keymap_unref(_keymap);
	_keymap = keymap;
	_xkb_state = xkb_state_new(keymap);
	update_keyboard_layout_german(keymap, 0);
}

void reshade::wayland_input::on_keyboard_enter(wl_surface *surface)
{
	release_keyboard();
	_keyboard_focus_surface = surface;
	_keyboard_focus_accepted = accepts_focus(surface, "keyboard");
	refresh_keyboard_focus();
}

void reshade::wayland_input::on_keyboard_leave()
{
	release_keyboard();
	_keyboard_focused = false;
	_keyboard_focus_surface = nullptr;
	_keyboard_focus_accepted = false;
}

void reshade::wayland_input::on_key(uint32_t serial, uint32_t key, uint32_t state)
{
	if (!_keyboard_focused || _xkb_state == nullptr)
		return;

	_last_serial = serial;
	// Wayland sends evdev codes, XKB keycodes are offset by 8.
	const xkb_keycode_t keycode = key + 8;
	const bool pressed = state == WL_KEYBOARD_KEY_STATE_PRESSED;
	if (const unsigned int virtual_key = virtual_key_from_keysym(xkb_state_key_get_one_sym(_xkb_state, keycode)))
		set_key(virtual_key, pressed);
	if (pressed)
		add_text(xkb_state_key_get_utf32(_xkb_state, keycode));
	xkb_state_update_key(_xkb_state, keycode, pressed ? XKB_KEY_DOWN : XKB_KEY_UP);
}

void reshade::wayland_input::on_modifiers(uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group)
{
	if (_xkb_state == nullptr)
		return;

	xkb_state_update_mask(_xkb_state, depressed, latched, locked, 0, 0, group);
	sync_modifiers();
	update_keyboard_layout_german(_keymap, group);
}

void reshade::wayland_input::on_pointer_enter(uint32_t serial, wl_surface *surface, double x, double y)
{
	release_pointer();
	// The capture layer covers the host surface at the same origin, and hides the host's cursor.
	if (surface != nullptr && surface == _overlay.surface())
	{
		_pointer_focused = true;
		if (_pointer_device != nullptr)
			wl_pointer_set_cursor(_pointer_device, serial, nullptr, 0, 0);
	}
	else
	{
		_pointer_focused = accepts_focus(surface, "pointer");
	}
	if (_pointer_focused)
		_pointer.enter(x, y);
	else
		_pointer.leave();
	refresh_keyboard_focus();
}

void reshade::wayland_input::on_pointer_leave()
{
	release_pointer();
	_pointer.leave();
	_pointer_focused = false;
	refresh_keyboard_focus();
}

void reshade::wayland_input::on_pointer_motion(double x, double y)
{
	if (_pointer_focused)
		_pointer.absolute_motion(x, y);
}

void reshade::wayland_input::on_pointer_button(uint32_t serial, uint32_t button, uint32_t state)
{
	if (!_pointer_focused)
		return;

	_last_serial = serial;
	if (const unsigned int key = virtual_key_from_evdev_button(button))
		set_key(key, state == WL_POINTER_BUTTON_STATE_PRESSED);
}

void reshade::wayland_input::on_pointer_axis(uint32_t axis, double value)
{
	if (_pointer_focused && axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
		_pointer.axis(value);
}

void reshade::wayland_input::on_pointer_axis_discrete(uint32_t axis, int32_t steps)
{
	if (_pointer_focused && axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
		_pointer.axis_discrete(steps);
}

void reshade::wayland_input::on_pointer_frame()
{
	add_wheel_delta(_pointer.end_axis_frame());
}

void reshade::wayland_input::on_relative_motion(double dx, double dy, uint64_t time)
{
	if (_pointer_focused)
		_pointer.relative_motion(dx, dy, time);
}

void reshade::wayland_input::on_overlay_active_changed()
{
	_pointer.set_overlay_active(overlay_active());
	log_pointer_changes();
}

void reshade::wayland_input::update_pointer_scale()
{
	// Wine maps its own coordinates, so they already match the swapchain.
	if (!_wine_host)
		_pointer.set_preferred_scale(_overlay.preferred());
}

void reshade::wayland_input::update_capture()
{
	// Size the layer by the lowest plausible scale, so it can never extend beyond the host surface.
	double scale = _wine_host ? _overlay.preferred() : _pointer.scale();
	if (scale < 1.0)
		scale = 1.0;
	const auto logical_width = static_cast<unsigned int>(width() / scale);
	const auto logical_height = static_cast<unsigned int>(height() / scale);
	const std::vector<pixel_rect> regions = to_pixel_rects(pointer_capture(), logical_width, logical_height);
	if (!_overlay.set_capture(regions, logical_width, logical_height))
		return;

	wl_display_flush(_display);
	const bool capturing = !regions.empty();
	if (capturing != _capturing)
		log::message(log::level::info, "Wayland surface %p input capture %s (%ux%u).", _surface, capturing ? "on" : "off", logical_width, logical_height);
	_capturing = capturing;
}

void reshade::wayland_input::publish_pointer()
{
	_pointer.end_batch();
	if (_pointer_focused)
		set_mouse_position(_pointer.x(), _pointer.y());
	log_pointer_changes();
}

void reshade::wayland_input::log_pointer_changes()
{
	if (_pointer.scale() != _logged_scale)
	{
		_logged_scale = _pointer.scale();
		log::message(log::level::info, "Wayland surface %p pointer scale=%.3f (%s).", _surface, _logged_scale,
			_pointer.scale_refuted() ? "host renders at 1:1, compositor preference ignored" : "compositor preference");
	}
	if (_pointer.current_mode() != _logged_mode)
	{
		constexpr const char *mode_names[] = { "passive", "host cursor", "overlay cursor (pointer locked)" };
		log::message(log::level::info, "Wayland surface %p pointer mode: %s -> %s.", _surface,
			mode_names[static_cast<int>(_logged_mode)], mode_names[static_cast<int>(_pointer.current_mode())]);
		_logged_mode = _pointer.current_mode();
	}
}

bool reshade::wayland_input::accepts_focus(wl_surface *focused_surface, const char *device) const
{
	const bool exact_match = focused_surface == _surface;
	// Wine presents through a surface of its own, so focus is on a different surface of the same
	// connection. Accept it when that connection has no other Vulkan surface it could belong to.
	const bool wine_match = !exact_match && _wine_host && count_windows_on_display(_display) == 1;

	log::message(log::level::info, "Wayland %s enter: focused_surface=%p vulkan_surface=%p exact_match=%d%s.", device, focused_surface, _surface, exact_match, wine_match ? " fallback=wine-single-surface" : "");
	return exact_match || wine_match;
}

void reshade::wayland_input::refresh_keyboard_focus()
{
	const bool focused = _keyboard_focus_surface != nullptr && (_keyboard_focus_accepted || _pointer_focused);
	if (_keyboard_focused && !focused)
		release_keyboard();
	_keyboard_focused = focused;
	sync_modifiers();
}

void reshade::wayland_input::sync_modifiers()
{
	if (!_keyboard_focused || _xkb_state == nullptr)
		return;

	const auto is_active = [this](const char *name) { return xkb_state_mod_name_is_active(_xkb_state, name, XKB_STATE_MODS_EFFECTIVE) > 0; };
	set_modifiers(is_active(XKB_MOD_NAME_CTRL), is_active(XKB_MOD_NAME_SHIFT), is_active(XKB_MOD_NAME_ALT));
}

void reshade::wayland_input::bind_relative_pointer()
{
	if (_relative_pointer_manager == nullptr || _pointer_device == nullptr || _relative_pointer != nullptr)
		return;

	_relative_pointer = zwp_relative_pointer_manager_v1_get_relative_pointer(_relative_pointer_manager, _pointer_device);
	set_queue(_relative_pointer, _queue);
	zwp_relative_pointer_v1_add_listener(_relative_pointer, &relative_pointer_listener, this);
}

void reshade::wayland_input::release_keyboard_device()
{
	if (_keyboard == nullptr)
		return;

	on_keyboard_leave();
	release_keyboard_proxy(_keyboard);
	_keyboard = nullptr;
}

void reshade::wayland_input::release_pointer_device()
{
	if (_pointer_device == nullptr)
		return;

	on_pointer_leave();
	if (_relative_pointer != nullptr)
		zwp_relative_pointer_v1_destroy(_relative_pointer);
	_relative_pointer = nullptr;
	release_pointer_proxy(_pointer_device);
	_pointer_device = nullptr;
}
