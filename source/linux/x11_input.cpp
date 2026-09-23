#include "x11_input.hpp"
#include "dll_log.hpp"
#include "key_translation.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <xcb/xfixes.h>
#include <xcb/xinput.h>

namespace
{
	// XCB replies, errors and events are malloc'ed and owned by the caller.
	template <typename T>
	auto owned(T *value) { return std::unique_ptr<T, decltype(&std::free)>(value, &std::free); }

	xcb_atom_t intern_atom(xcb_connection_t *connection, const char *name)
	{
		const auto atom = owned(xcb_intern_atom_reply(connection, xcb_intern_atom(connection, false, static_cast<uint16_t>(std::strlen(name)), name), nullptr));
		return atom != nullptr ? atom->atom : static_cast<xcb_atom_t>(XCB_ATOM_NONE);
	}

	void raw_motion_delta(const xcb_input_raw_motion_event_t &event, double delta[2])
	{
		// Raw events only carry the valuators that changed, in axis order.
		const xcb_input_fp3232_t *const values = xcb_input_raw_button_press_axisvalues_raw(&event);
		const uint32_t *const mask = xcb_input_raw_button_press_valuator_mask(&event);
		unsigned int value_index = 0;
		for (unsigned int axis = 0; axis < event.valuators_len * 32u; ++axis)
		{
			if ((mask[axis / 32] & (1u << (axis % 32))) == 0)
				continue;
			if (axis < 2)
				delta[axis] = values[value_index].integral + values[value_index].frac / 4294967296.0;
			++value_index;
		}
	}
}

reshade::x11_input::x11_input(input &owner, xcb_window_t window, void *wsi_display, input::wsi_kind wsi_kind) :
	input_backend(owner),
	_window(window),
	_wsi_display(wsi_display),
	_wsi_kind(wsi_kind)
{
}

reshade::x11_input::~x11_input()
{
	if (_connection == nullptr)
		return;

	set_native_cursor_hidden(false);
	xcb_disconnect(_connection);
}

bool reshade::x11_input::initialize()
{
	int screen = 0;
	_connection = xcb_connect(nullptr, &screen);
	if (_connection == nullptr || xcb_connection_has_error(_connection) != 0)
		return false;

	xcb_screen_iterator_t screen_iterator = xcb_setup_roots_iterator(xcb_get_setup(_connection));
	for (int i = 0; i < screen && screen_iterator.rem != 0; ++i)
		xcb_screen_next(&screen_iterator);
	if (screen_iterator.rem == 0)
		return false;
	_root = screen_iterator.data->root;

	if (owned(xcb_get_geometry_reply(_connection, xcb_get_geometry(_connection, _window), nullptr)) == nullptr)
	{
		log::message(log::level::warning, "X11 input window %#x is not present on the server selected by DISPLAY (WSI display=%p kind=%s).", _window, _wsi_display, _wsi_kind == input::wsi_kind::xcb ? "xcb" : "xlib");
		return false;
	}

	_wm_state_atom = intern_atom(_connection, "_NET_WM_STATE");
	_fullscreen_atom = intern_atom(_connection, "_NET_WM_STATE_FULLSCREEN");

	const xcb_query_extension_reply_t *const xinput_extension = xcb_get_extension_data(_connection, &xcb_input_id);
	if (xinput_extension == nullptr || !xinput_extension->present)
		return false;
	_xinput_opcode = xinput_extension->major_opcode;

	// XInput 2.0 suppresses raw events while another client (the host) holds a grab, 2.1 does not.
	const auto version = owned(xcb_input_xi_query_version_reply(_connection, xcb_input_xi_query_version(_connection, 2, 1), nullptr));
	if (version == nullptr || version->major_version < 2)
		return false;
	log::message(log::level::info, "X11 input: negotiated XInput %u.%u.", version->major_version, version->minor_version);
	if (version->major_version == 2 && version->minor_version == 0)
		log::message(log::level::warning, "XInput 2.1 is unavailable; input cannot be observed during host grabs.");

	const xcb_query_extension_reply_t *const xfixes_extension = xcb_get_extension_data(_connection, &xcb_xfixes_id);
	if (xfixes_extension != nullptr && xfixes_extension->present)
	{
		const auto xfixes_version = owned(xcb_xfixes_query_version_reply(_connection, xcb_xfixes_query_version(_connection, 4, 0), nullptr));
		_xfixes_cursor_hiding = xfixes_version != nullptr && xfixes_version->major_version >= 4;
	}

	struct
	{
		xcb_input_event_mask_t header;
		uint32_t mask;
	} raw_events = {
		{ XCB_INPUT_DEVICE_ALL_MASTER, 1 },
		XCB_INPUT_XI_EVENT_MASK_RAW_KEY_PRESS | XCB_INPUT_XI_EVENT_MASK_RAW_KEY_RELEASE |
		XCB_INPUT_XI_EVENT_MASK_RAW_BUTTON_PRESS | XCB_INPUT_XI_EVENT_MASK_RAW_BUTTON_RELEASE |
		XCB_INPUT_XI_EVENT_MASK_RAW_MOTION
	};
	if (const auto error = owned(xcb_request_check(_connection, xcb_input_xi_select_events_checked(_connection, _root, 1, &raw_events.header))))
	{
		log::message(log::level::warning, "XInput2 raw event subscription failed root=%#x error=%u.", _root, error->error_code);
		return false;
	}
	xcb_flush(_connection);

	_wine_available = _wine.initialize();
	// X11 does not replay focus events that happened before this connection subscribed.
	refresh_keyboard_focus();

	const bool keyboard_ready = cache_key_translations();
	log::message(log::level::info, "X11 input: xcb_window=%#x keyboard=%s pointer=%s cursor_hiding=%s translation=xcb-cached.",
		_window, keyboard_ready ? "yes" : "no", _wine_available ? "wine-win32u" : "xinput2", _wine.cursor_hiding_available() ? "wine-win32u" : (_xfixes_cursor_hiding ? "xfixes" : "no"));
	return keyboard_ready;
}

void reshade::x11_input::next_frame()
{
	// XI2 raw events are global, so establish ownership once before draining this frame's events.
	refresh_keyboard_focus();
	if (!_wine_available)
		_fullscreen = overlay_active() && query_fullscreen();
	query_pointer();
	if (_wine_available)
	{
		query_wine_buttons();
		if (_native_cursor_hidden)
			_wine.maintain_hidden_cursor();
	}

	while (xcb_generic_event_t *const event = xcb_poll_for_event(_connection))
	{
		const auto owned_event = owned(event);
		// This connection only subscribes to XI2 raw events, which arrive as generic events.
		if ((event->response_type & 0x7F) != XCB_GE_GENERIC)
			continue;
		const auto *const generic = reinterpret_cast<const xcb_ge_generic_event_t *>(event);
		if (generic->extension != _xinput_opcode)
			continue;

		const auto *const raw = reinterpret_cast<const xcb_input_raw_key_press_event_t *>(event);
		switch (generic->event_type)
		{
		case XCB_INPUT_RAW_KEY_PRESS:
		case XCB_INPUT_RAW_KEY_RELEASE:
			on_raw_key(static_cast<xcb_keycode_t>(raw->detail), generic->event_type == XCB_INPUT_RAW_KEY_PRESS);
			break;
		case XCB_INPUT_RAW_BUTTON_PRESS:
		case XCB_INPUT_RAW_BUTTON_RELEASE:
			on_raw_button(raw->detail, generic->event_type == XCB_INPUT_RAW_BUTTON_PRESS);
			break;
		case XCB_INPUT_RAW_MOTION:
		{
			double delta[2] = {};
			raw_motion_delta(*reinterpret_cast<const xcb_input_raw_motion_event_t *>(event), delta);
			on_raw_motion(delta[0], delta[1]);
			break;
		}
		}
	}
}

uint32_t reshade::x11_input::keysym_to_utf32(xcb_keysym_t keysym)
{
	// Latin-1 keysyms equal their code point, Unicode keysyms carry it with a 0x01000000 prefix.
	if ((keysym >= 0x20 && keysym <= 0x7E) || (keysym >= 0xA0 && keysym <= 0xFF))
		return keysym;
	if ((keysym & 0xFF000000u) == 0x01000000u)
		return keysym & 0x00FFFFFFu;
	return 0;
}

xcb_window_t reshade::x11_input::select_keyboard_window(xcb_window_t surface_window, xcb_window_t focused_window, bool surface_related, bool wine_related)
{
	if (focused_window == XCB_WINDOW_NONE || focused_window == XCB_INPUT_FOCUS_POINTER_ROOT)
		return XCB_WINDOW_NONE;
	if (surface_related)
		return surface_window;
	return wine_related ? focused_window : static_cast<xcb_window_t>(XCB_WINDOW_NONE);
}

void reshade::x11_input::on_raw_key(xcb_keycode_t keycode, bool pressed)
{
	if (!_keyboard_focused)
		return;

	const key_translation &translation = _key_translations[keycode];
	if (const unsigned int virtual_key = virtual_key_from_keysym(translation.keysym))
		set_key(virtual_key, pressed);

	if (pressed && !is_key_down(input::key_ctrl) && !is_key_down(input::key_alt))
		add_text(is_key_down(input::key_shift) && translation.shifted_utf32 != 0 ? translation.shifted_utf32 : translation.utf32);
}

void reshade::x11_input::on_raw_button(uint32_t button, bool pressed)
{
	if (!_pointer_focused || !_keyboard_focused)
		return;

	// Buttons 4 and 5 are the vertical wheel.
	if (button == 4 || button == 5)
	{
		if (pressed)
			add_wheel_delta(button == 4 ? 1 : -1);
	}
	else if (const unsigned int key = virtual_key_from_x11_button(button))
	{
		set_key(key, pressed);
	}
}

void reshade::x11_input::on_raw_motion(double dx, double dy)
{
	if (!_keyboard_focused || !_pointer_focused || !uses_relative_motion())
		return;

	const auto offset = [](unsigned int position, double delta) { return static_cast<unsigned int>(std::max(0L, std::lround(position + delta))); };
	set_mouse_position(offset(_owner.mouse_position_x(), dx), offset(_owner.mouse_position_y(), dy));
}

void reshade::x11_input::update_cursor_policy()
{
	set_native_cursor_hidden(overlay_active());
}

bool reshade::x11_input::cache_key_translations()
{
	const xcb_setup_t *const setup = xcb_get_setup(_connection);
	const uint8_t count = static_cast<uint8_t>(setup->max_keycode - setup->min_keycode + 1);
	const auto mapping = owned(xcb_get_keyboard_mapping_reply(_connection, xcb_get_keyboard_mapping(_connection, setup->min_keycode, count), nullptr));
	if (mapping == nullptr || mapping->keysyms_per_keycode == 0)
		return false;

	const xcb_keysym_t *const keysyms = xcb_get_keyboard_mapping_keysyms(mapping.get());
	for (unsigned int index = 0; index < count; ++index)
	{
		const xcb_keysym_t base = keysyms[index * mapping->keysyms_per_keycode];
		const xcb_keysym_t shifted = mapping->keysyms_per_keycode > 1 ? keysyms[index * mapping->keysyms_per_keycode + 1] : base;
		_key_translations[setup->min_keycode + index] = { base != XCB_NO_SYMBOL ? base : shifted, keysym_to_utf32(base), keysym_to_utf32(shifted) };
	}
	return true;
}

bool reshade::x11_input::is_ancestor_window(xcb_window_t ancestor, xcb_window_t window) const
{
	while (window != XCB_WINDOW_NONE && window != XCB_INPUT_FOCUS_POINTER_ROOT)
	{
		if (window == ancestor)
			return true;

		const auto tree = owned(xcb_query_tree_reply(_connection, xcb_query_tree(_connection, window), nullptr));
		if (tree == nullptr || tree->parent == window)
			break;
		window = tree->parent;
	}
	return false;
}

bool reshade::x11_input::contains_window(xcb_window_t window) const
{
	if (window == XCB_WINDOW_NONE || window == XCB_INPUT_FOCUS_POINTER_ROOT || window == _root)
		return false;
	// Toolkits may focus a parent of the Vulkan window (Qt) or one of its children (Wine).
	return is_ancestor_window(_window, window) || is_ancestor_window(window, _window);
}

void reshade::x11_input::refresh_keyboard_focus()
{
	const auto focus = owned(xcb_get_input_focus_reply(_connection, xcb_get_input_focus(_connection), nullptr));
	if (focus == nullptr)
		return;

	// Walking the window tree is expensive, so only re-evaluate ancestry when focus moved.
	const bool surface_related = focus->focus != _last_observed_focus ? contains_window(focus->focus) : _keyboard_window == _window;
	_last_observed_focus = focus->focus;
	const bool wine_related = _wine_available && _wine.is_foreground_process();
	_keyboard_window = select_keyboard_window(_window, focus->focus, surface_related, wine_related);

	const bool focused = _keyboard_window != XCB_WINDOW_NONE;
	if (_keyboard_focused && !focused)
		release_keyboard();
	_keyboard_focused = focused;
}

bool reshade::x11_input::query_fullscreen() const
{
	if (_wm_state_atom == XCB_ATOM_NONE || _fullscreen_atom == XCB_ATOM_NONE)
		return false;

	// The window manager sets the state on the top-level window, which may be a parent (Qt).
	xcb_window_t window = _window;
	for (unsigned int depth = 0; window != XCB_WINDOW_NONE && window != _root && depth < 64; ++depth)
	{
		const auto property = owned(xcb_get_property_reply(_connection, xcb_get_property(_connection, false, window, _wm_state_atom, XCB_ATOM_ATOM, 0, 64), nullptr));
		if (property != nullptr && property->type == XCB_ATOM_ATOM && property->format == 32)
		{
			const auto *const states = static_cast<const xcb_atom_t *>(xcb_get_property_value(property.get()));
			const auto *const states_end = states + xcb_get_property_value_length(property.get()) / sizeof(xcb_atom_t);
			if (std::find(states, states_end, _fullscreen_atom) != states_end)
				return true;
		}

		const auto tree = owned(xcb_query_tree_reply(_connection, xcb_query_tree(_connection, window), nullptr));
		if (tree == nullptr || tree->parent == window)
			break;
		window = tree->parent;
	}
	return false;
}

bool reshade::x11_input::uses_relative_motion() const
{
	return overlay_active() && (_wine_available || _fullscreen);
}

void reshade::x11_input::query_pointer()
{
	wine_input_bridge::point position = {};
	bool focused = false;
	const bool from_wine = _wine.query_pointer_position(position, width(), height(), focused);
	if (!from_wine)
	{
		const auto pointer = owned(xcb_query_pointer_reply(_connection, xcb_query_pointer(_connection, _window), nullptr));
		if (pointer == nullptr)
			return;
		position = { pointer->win_x, pointer->win_y };
		focused = pointer->same_screen && pointer->win_x >= 0 && pointer->win_y >= 0 && pointer->win_x < static_cast<int>(width()) && pointer->win_y < static_cast<int>(height());
	}

	if (_pointer_focused && !focused)
		release_pointer();
	_pointer_focused = focused;
	if (focused && (from_wine || !uses_relative_motion()))
		set_mouse_position(static_cast<unsigned int>(position.x), static_cast<unsigned int>(position.y));
}

void reshade::x11_input::query_wine_buttons()
{
	if (!_wine.available() || !_keyboard_focused)
		return;

	// Wine's virtual-key codes for mouse buttons are the ones ReShade uses.
	for (const unsigned int key : mouse_keys)
		set_key(key, _wine.button_down(static_cast<int>(key)));
}

void reshade::x11_input::set_native_cursor_hidden(bool hidden)
{
	if (_native_cursor_hidden == hidden)
		return;

	if (_wine.cursor_hiding_available())
	{
		_wine.set_cursor_hidden(hidden);
		_native_cursor_hidden = hidden;
		return;
	}
	if (!_xfixes_cursor_hiding)
		return;

	const xcb_void_cookie_t cookie = hidden ? xcb_xfixes_hide_cursor_checked(_connection, _window) : xcb_xfixes_show_cursor_checked(_connection, _window);
	if (const auto error = owned(xcb_request_check(_connection, cookie)))
	{
		log::message(log::level::warning, "XFixes cursor %s failed for window %#x with error %u.", hidden ? "hide" : "show", _window, error->error_code);
		return;
	}
	_native_cursor_hidden = hidden;
	xcb_flush(_connection);
}
