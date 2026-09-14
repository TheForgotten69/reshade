#pragma once

#include "input.hpp"
#include "dll_log.hpp"
#include "key_translation.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <dlfcn.h>
#include <unistd.h>

#include <xcb/xcb.h>
#include <xcb/xfixes.h>
#include <xcb/xinput.h>

// Owns input state for a single X11/XWayland Vulkan window. Native clients use a private XCB
// connection and XInput2 raw events, while Wine clients dynamically use win32u for pointer state
// when Wine exposes only an off-screen X11 Vulkan window. The Wine bridge is optional and does not
// introduce a link-time dependency.
struct reshade::x11_input_context
{
	input *owner = nullptr;
	xcb_window_t window = XCB_WINDOW_NONE;
	xcb_window_t keyboard_window = XCB_WINDOW_NONE;
	xcb_window_t last_observed_focus = XCB_WINDOW_NONE;
	xcb_window_t root = XCB_WINDOW_NONE;
	xcb_connection_t *connection = nullptr;
	uint8_t xinput_opcode = 0;
	xcb_atom_t net_wm_pid_atom = XCB_ATOM_NONE;
	bool wine_input_available = false;
	bool xfixes_cursor_hiding = false;
	bool native_cursor_hidden = false;
	struct wine_point
	{
		int32_t x;
		int32_t y;
	};
	using wine_get_cursor_pos_fn = int (*)(wine_point *);
	using wine_get_foreground_window_fn = void *(*)();
	using wine_call_hwnd_param_fn = uintptr_t (*)(void *, uintptr_t, uint32_t);
	using wine_get_async_key_state_fn = int16_t (*)(int);
	using wine_get_cursor_fn = void *(*)();
	using wine_set_cursor_fn = void *(*)(void *);
	wine_get_cursor_pos_fn wine_get_cursor_pos = nullptr;
	wine_get_foreground_window_fn wine_get_foreground_window = nullptr;
	wine_call_hwnd_param_fn wine_call_hwnd_param = nullptr;
	wine_get_async_key_state_fn wine_get_async_key_state = nullptr;
	wine_get_cursor_fn wine_get_cursor = nullptr;
	wine_set_cursor_fn wine_set_cursor = nullptr;
	void *saved_wine_cursor = nullptr;
	struct cached_key
	{
		xcb_keysym_t keysym = XKB_KEY_NoSymbol;
		uint32_t utf32 = 0;
		uint32_t shifted_utf32 = 0;
	};
	std::array<cached_key, 256> key_translation = {};
	bool keyboard_focused = false;
	bool pointer_focused = false;
	unsigned int width = 1;
	unsigned int height = 1;

	~x11_input_context()
	{
		if (connection != nullptr)
		{
			set_native_cursor_hidden(false);
			xcb_disconnect(connection);
		}
	}
	void clear_keyboard_state()
	{
		for (unsigned int key = input::key_button_xbutton2 + 1; key < std::size(owner->_keys); ++key)
			if ((owner->_keys[key] & 0x80) != 0)
				owner->_keys[key] = 0x08;
	}
	void clear_pointer_state()
	{
		constexpr unsigned int keys[] = {input::key_button_left, input::key_button_right, input::key_button_middle, input::key_button_xbutton1, input::key_button_xbutton2};
		for (const unsigned int key : keys)
			if ((owner->_keys[key] & 0x80) != 0)
				owner->_keys[key] = 0x08;
	}
	void set_key(xcb_keycode_t key, bool pressed)
	{
		const cached_key &translation = key_translation[key];
		const xkb_keysym_t keysym = translation.keysym;
		const unsigned int virtual_key = virtual_key_from_keysym(keysym);
		if (virtual_key != 0)
		{
			owner->_keys[virtual_key] = pressed ? 0x88 : 0x08;
			if (virtual_key == input::key_left_ctrl || virtual_key == input::key_right_ctrl)
				owner->_keys[input::key_ctrl] = (owner->_keys[input::key_left_ctrl] & 0x80) != 0 || (owner->_keys[input::key_right_ctrl] & 0x80) != 0 ? 0x88 : 0x08;
			if (virtual_key == input::key_left_shift || virtual_key == input::key_right_shift)
				owner->_keys[input::key_shift] = (owner->_keys[input::key_left_shift] & 0x80) != 0 || (owner->_keys[input::key_right_shift] & 0x80) != 0 ? 0x88 : 0x08;
			if (virtual_key == input::key_left_alt || virtual_key == input::key_right_alt)
				owner->_keys[input::key_alt] = (owner->_keys[input::key_left_alt] & 0x80) != 0 || (owner->_keys[input::key_right_alt] & 0x80) != 0 ? 0x88 : 0x08;
		}
		if (pressed)
		{
			const bool shifted = (owner->_keys[input::key_shift] & 0x80) != 0;
			const bool modified = (owner->_keys[input::key_ctrl] & 0x80) != 0 || (owner->_keys[input::key_alt] & 0x80) != 0;
			const uint32_t utf32 = modified ? 0 : (shifted && translation.shifted_utf32 != 0 ? translation.shifted_utf32 : translation.utf32);
			if (utf32 != 0 && utf32 <= 0x10ffff && (utf32 < 0xd800 || utf32 > 0xdfff))
				owner->_text_input += static_cast<wchar_t>(utf32);
		}
	}
	void handle_raw_key(xcb_keycode_t key, bool pressed)
	{
		if (keyboard_focused)
			set_key(key, pressed);
	}
	static uint32_t keysym_to_utf32(xcb_keysym_t keysym)
	{
		if ((keysym >= 0x20 && keysym <= 0x7e) || (keysym >= 0xa0 && keysym <= 0xff))
			return keysym;
		if ((keysym & 0xff000000u) == 0x01000000u)
			return keysym & 0x00ffffffu;
		return 0;
	}
	bool initialize_wine_input_bridge()
	{
		void *const module = dlopen("win32u.so", RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD);
		auto lookup = [module](const char *name) { return dlsym(module != nullptr ? module : RTLD_DEFAULT, name); };
		wine_get_cursor_pos = reinterpret_cast<wine_get_cursor_pos_fn>(lookup("NtUserGetCursorPos"));
		wine_get_foreground_window = reinterpret_cast<wine_get_foreground_window_fn>(lookup("NtUserGetForegroundWindow"));
		wine_call_hwnd_param = reinterpret_cast<wine_call_hwnd_param_fn>(lookup("NtUserCallHwndParam"));
		wine_get_async_key_state = reinterpret_cast<wine_get_async_key_state_fn>(lookup("NtUserGetAsyncKeyState"));
		wine_get_cursor = reinterpret_cast<wine_get_cursor_fn>(lookup("NtUserGetCursor"));
		wine_set_cursor = reinterpret_cast<wine_set_cursor_fn>(lookup("NtUserSetCursor"));
		if (module != nullptr)
			dlclose(module);
		return wine_get_cursor_pos != nullptr && wine_get_foreground_window != nullptr && wine_call_hwnd_param != nullptr;
	}
	bool query_wine_pointer_position()
	{
		if (wine_get_cursor_pos == nullptr || wine_get_foreground_window == nullptr || wine_call_hwnd_param == nullptr)
			return false;

		wine_point position = {};
		void *const foreground_window = wine_get_foreground_window();
		// NtUserCallHwndParam_ScreenToClient is Wine internal operation 23. Resolve the
		// function dynamically and use it only when running inside Wine's win32u module.
		if (foreground_window == nullptr || !wine_get_cursor_pos(&position) ||
			wine_call_hwnd_param(foreground_window, reinterpret_cast<uintptr_t>(&position), 23) == 0)
			return false;

		const bool was_focused = pointer_focused;
		pointer_focused = position.x >= 0 && position.y >= 0 && position.x < static_cast<int32_t>(width) && position.y < static_cast<int32_t>(height);
		if (was_focused && !pointer_focused)
			clear_pointer_state();
		if (pointer_focused)
		{
			owner->_mouse_position[0] = static_cast<unsigned int>(position.x);
			owner->_mouse_position[1] = static_cast<unsigned int>(position.y);
		}
		return true;
	}
	void poll_wine_mouse_buttons()
	{
		if (wine_get_async_key_state == nullptr || !keyboard_focused)
			return;
		constexpr std::pair<int, unsigned int> buttons[] = {
			{0x01, input::key_button_left}, {0x02, input::key_button_right}, {0x04, input::key_button_middle},
			{0x05, input::key_button_xbutton1}, {0x06, input::key_button_xbutton2}};
		for (const auto &[virtual_key, input_key] : buttons)
			owner->_keys[input_key] = (wine_get_async_key_state(virtual_key) & 0x8000) != 0 ? 0x88 : 0x08;
	}
	void set_native_cursor_hidden(bool hidden)
	{
		if (native_cursor_hidden == hidden)
			return;
		if (wine_get_cursor != nullptr && wine_set_cursor != nullptr)
		{
			if (hidden)
			{
				saved_wine_cursor = wine_get_cursor();
				wine_set_cursor(nullptr);
			}
			else if (saved_wine_cursor != nullptr)
			{
				wine_set_cursor(saved_wine_cursor);
				saved_wine_cursor = nullptr;
			}
			native_cursor_hidden = hidden;
			return;
		}
		if (!xfixes_cursor_hiding)
			return;

		const xcb_void_cookie_t cookie = hidden ? xcb_xfixes_hide_cursor_checked(connection, window) : xcb_xfixes_show_cursor_checked(connection, window);
		xcb_generic_error_t *const error = xcb_request_check(connection, cookie);
		if (error != nullptr)
		{
			reshade::log::message(reshade::log::level::warning, "XFixes cursor %s failed for window %#x with error %u.", hidden ? "hide" : "show", window, error->error_code);
			free(error);
			return;
		}

		native_cursor_hidden = hidden;
		xcb_flush(connection);
	}
	xcb_window_t pointer_query_window() const
	{
		return window;
	}
	bool cache_key_translation()
	{
		const xcb_setup_t *const setup = xcb_get_setup(connection);
		const uint8_t count = static_cast<uint8_t>(setup->max_keycode - setup->min_keycode + 1);
		xcb_get_keyboard_mapping_reply_t *const mapping = xcb_get_keyboard_mapping_reply(connection, xcb_get_keyboard_mapping(connection, setup->min_keycode, count), nullptr);
		if (mapping == nullptr || mapping->keysyms_per_keycode == 0)
		{
			free(mapping);
			return false;
		}
		const xcb_keysym_t *const symbols = xcb_get_keyboard_mapping_keysyms(mapping);
		for (unsigned int index = 0; index < count; ++index)
		{
			const xcb_keycode_t key = static_cast<xcb_keycode_t>(setup->min_keycode + index);
			const xcb_keysym_t base = symbols[index * mapping->keysyms_per_keycode];
			const xcb_keysym_t shifted = mapping->keysyms_per_keycode > 1 ? symbols[index * mapping->keysyms_per_keycode + 1] : base;
			key_translation[key] = {base != XCB_NO_SYMBOL ? base : shifted, keysym_to_utf32(base), keysym_to_utf32(shifted)};
		}
		free(mapping);
		return true;
	}
	void set_pointer_position(const xcb_motion_notify_event_t &event)
	{
		if (!pointer_focused)
			return;
		owner->_mouse_position[0] = static_cast<unsigned int>(std::clamp<int>(event.event_x, 0, static_cast<int>(std::max(1u, width))));
		owner->_mouse_position[1] = static_cast<unsigned int>(std::clamp<int>(event.event_y, 0, static_cast<int>(std::max(1u, height))));
	}
	void handle_button(const xcb_button_press_event_t &event, bool pressed)
	{
		if (!pointer_focused)
			return;
		unsigned int key = 0;
		switch (event.detail)
		{
		case 1: key = input::key_button_left; break;
		case 2: key = input::key_button_middle; break;
		case 3: key = input::key_button_right; break;
		case 8: key = input::key_button_xbutton1; break;
		case 9: key = input::key_button_xbutton2; break;
		case 4: if (pressed) ++owner->_mouse_wheel_delta; return;
		case 5: if (pressed) --owner->_mouse_wheel_delta; return;
		default: return;
		}
		owner->_keys[key] = pressed ? 0x88 : 0x08;
	}
	bool contains_window(xcb_window_t focused_window) const
	{
		// The Vulkan surface may belong to a parent of the actual X11 input window
		// (this is common with Wine). Walk towards the root so late attachment works
		// for both the surface window itself and one of its children.
		while (focused_window != XCB_WINDOW_NONE && focused_window != XCB_INPUT_FOCUS_POINTER_ROOT)
		{
			if (focused_window == window)
				return true;

			xcb_query_tree_reply_t *const tree = xcb_query_tree_reply(connection, xcb_query_tree(connection, focused_window), nullptr);
			if (tree == nullptr)
				break;
			const xcb_window_t parent = tree->parent;
			free(tree);
			if (parent == focused_window)
				break;
			focused_window = parent;
		}
		return false;
	}
	bool belongs_to_current_process(xcb_window_t candidate) const
	{
		if (net_wm_pid_atom == XCB_ATOM_NONE)
			return false;
		while (candidate != XCB_WINDOW_NONE && candidate != XCB_INPUT_FOCUS_POINTER_ROOT)
		{
			xcb_get_property_reply_t *const property = xcb_get_property_reply(connection,
				xcb_get_property(connection, false, candidate, net_wm_pid_atom, XCB_ATOM_CARDINAL, 0, 1), nullptr);
			if (property != nullptr)
			{
				const bool matches = property->type == XCB_ATOM_CARDINAL && property->format == 32 &&
					xcb_get_property_value_length(property) == sizeof(uint32_t) &&
					*static_cast<const uint32_t *>(xcb_get_property_value(property)) == static_cast<uint32_t>(getpid());
				free(property);
				if (matches)
					return true;
			}

			xcb_query_tree_reply_t *const tree = xcb_query_tree_reply(connection, xcb_query_tree(connection, candidate), nullptr);
			if (tree == nullptr)
				break;
			const xcb_window_t parent = tree->parent;
			free(tree);
			if (parent == candidate)
				break;
			candidate = parent;
		}
		return false;
	}
	static xcb_window_t select_keyboard_window(xcb_window_t surface_window, xcb_window_t focused_window, bool surface_related, bool wine_related)
	{
		if (focused_window == XCB_WINDOW_NONE || focused_window == XCB_INPUT_FOCUS_POINTER_ROOT)
			return XCB_WINDOW_NONE;
		if (surface_related)
			return surface_window;
		return wine_related ? focused_window : XCB_WINDOW_NONE;
	}
	bool resolve_keyboard_focus(xcb_window_t focused_window)
	{
		if (focused_window == last_observed_focus)
			return keyboard_focused;
		last_observed_focus = focused_window;
		const bool surface_related = contains_window(focused_window);
		const bool wine_related = wine_input_available && belongs_to_current_process(focused_window);
		keyboard_window = select_keyboard_window(window, focused_window, surface_related, wine_related);
		return keyboard_window != XCB_WINDOW_NONE;
	}
	void query_initial_focus()
	{
		xcb_get_input_focus_reply_t *const focus = xcb_get_input_focus_reply(connection, xcb_get_input_focus(connection), nullptr);
		if (focus != nullptr)
		{
			keyboard_focused = resolve_keyboard_focus(focus->focus);
			free(focus);
		}
	}
	bool refresh_keyboard_focus()
	{
		xcb_get_input_focus_reply_t *const focus = xcb_get_input_focus_reply(connection, xcb_get_input_focus(connection), nullptr);
		if (focus == nullptr)
			return keyboard_focused;
		const bool was_focused = keyboard_focused;
		keyboard_focused = resolve_keyboard_focus(focus->focus);
		free(focus);
		if (was_focused && !keyboard_focused)
			clear_keyboard_state();
		return keyboard_focused;
	}
	void query_pointer_position()
	{
		if (query_wine_pointer_position())
			return;
		xcb_query_pointer_reply_t *const pointer = xcb_query_pointer_reply(connection, xcb_query_pointer(connection, pointer_query_window()), nullptr);
		if (pointer == nullptr)
			return;
		const bool was_focused = pointer_focused;
		pointer_focused = pointer->same_screen && pointer->win_x >= 0 && pointer->win_y >= 0 &&
			pointer->win_x < static_cast<int>(width) && pointer->win_y < static_cast<int>(height);
		if (was_focused && !pointer_focused)
			clear_pointer_state();
		if (pointer_focused && !owner->_block_cursor_warping)
		{
			owner->_mouse_position[0] = static_cast<unsigned int>(pointer->win_x);
			owner->_mouse_position[1] = static_cast<unsigned int>(pointer->win_y);
		}
		free(pointer);
	}
	void handle_raw_motion(const xcb_input_raw_motion_event_t &event)
	{
		if (!pointer_focused || !owner->_block_cursor_warping)
			return;
		// xcb_input_raw_motion_event_t is an alias of the generated raw-button event type.
		const xcb_input_fp3232_t *const values = xcb_input_raw_button_press_axisvalues_raw(&event);
		const uint32_t *const valuators = xcb_input_raw_button_press_valuator_mask(&event);
		double delta[2] = {};
		unsigned int value_index = 0;
		for (unsigned int axis = 0; axis < event.valuators_len * 32; ++axis)
		{
			if ((valuators[axis / 32] & (1u << (axis % 32))) == 0)
				continue;
			if (axis < 2)
				delta[axis] = values[value_index].integral + values[value_index].frac / 4294967296.0;
			++value_index;
		}
		owner->_mouse_position[0] = static_cast<unsigned int>(std::clamp(static_cast<int>(std::lround(owner->_mouse_position[0] + delta[0])), 0, static_cast<int>(width)));
		owner->_mouse_position[1] = static_cast<unsigned int>(std::clamp(static_cast<int>(std::lround(owner->_mouse_position[1] + delta[1])), 0, static_cast<int>(height)));
	}
	void next_frame()
	{
		// XI2 raw events are global, so establish ownership once before draining this frame's
		// batch. This bounds synchronous X requests independently of mouse polling rate.
		refresh_keyboard_focus();
		query_pointer_position();
		if (wine_input_available)
		{
			poll_wine_mouse_buttons();
			if (native_cursor_hidden && wine_set_cursor != nullptr)
				wine_set_cursor(nullptr);
		}
		while (xcb_generic_event_t *event = xcb_poll_for_event(connection))
		{
			if ((event->response_type & 0x7f) == XCB_GE_GENERIC)
			{
				auto *const generic = reinterpret_cast<xcb_ge_generic_event_t *>(event);
				if (generic->extension == xinput_opcode)
				{
					auto *const raw = reinterpret_cast<xcb_input_raw_key_press_event_t *>(event);
					switch (generic->event_type)
					{
					case XCB_INPUT_RAW_KEY_PRESS:
					case XCB_INPUT_RAW_KEY_RELEASE:
						handle_raw_key(static_cast<xcb_keycode_t>(raw->detail), generic->event_type == XCB_INPUT_RAW_KEY_PRESS);
						break;
					case XCB_INPUT_RAW_BUTTON_PRESS:
					case XCB_INPUT_RAW_BUTTON_RELEASE:
						if (pointer_focused && keyboard_focused)
						{
							xcb_button_press_event_t button = {};
							button.detail = static_cast<uint8_t>(raw->detail);
							handle_button(button, generic->event_type == XCB_INPUT_RAW_BUTTON_PRESS);
						}
						break;
					case XCB_INPUT_RAW_MOTION:
						if (keyboard_focused)
							handle_raw_motion(*reinterpret_cast<xcb_input_raw_motion_event_t *>(event));
						break;
					}
				}
				free(event);
				continue;
			}
			// This private connection subscribes only to XI2 raw events. Core events would require
			// selecting masks on the application's window and are intentionally not consumed here.
			free(event);
		}
	}
	bool initialize()
	{
		int screen = 0;
		connection = xcb_connect(nullptr, &screen);
		if (connection == nullptr || xcb_connection_has_error(connection) != 0)
			return false;

		xcb_screen_iterator_t screen_iterator = xcb_setup_roots_iterator(xcb_get_setup(connection));
		for (int i = 0; i < screen && screen_iterator.rem != 0; ++i)
			xcb_screen_next(&screen_iterator);
		if (screen_iterator.rem == 0)
			return false;
		root = screen_iterator.data->root;

		const xcb_query_extension_reply_t *const extension = xcb_get_extension_data(connection, &xcb_input_id);
		if (extension == nullptr || !extension->present)
			return false;
		xinput_opcode = extension->major_opcode;
		xcb_input_xi_query_version_reply_t *const version = xcb_input_xi_query_version_reply(connection, xcb_input_xi_query_version(connection, 2, 0), nullptr);
		if (version == nullptr || version->major_version < 2)
		{
			free(version);
			return false;
		}
		free(version);

		const xcb_query_extension_reply_t *const xfixes_extension = xcb_get_extension_data(connection, &xcb_xfixes_id);
		if (xfixes_extension != nullptr && xfixes_extension->present)
		{
			xcb_xfixes_query_version_reply_t *const xfixes_version = xcb_xfixes_query_version_reply(connection, xcb_xfixes_query_version(connection, 4, 0), nullptr);
			xfixes_cursor_hiding = xfixes_version != nullptr && xfixes_version->major_version >= 4;
			free(xfixes_version);
		}

		struct
		{
			xcb_input_event_mask_t header;
			uint32_t mask;
		} raw_events = {{XCB_INPUT_DEVICE_ALL_MASTER, 1},
			XCB_INPUT_XI_EVENT_MASK_RAW_KEY_PRESS | XCB_INPUT_XI_EVENT_MASK_RAW_KEY_RELEASE |
			XCB_INPUT_XI_EVENT_MASK_RAW_BUTTON_PRESS | XCB_INPUT_XI_EVENT_MASK_RAW_BUTTON_RELEASE |
			XCB_INPUT_XI_EVENT_MASK_RAW_MOTION};
		const xcb_void_cookie_t select_cookie = xcb_input_xi_select_events_checked(connection, root, 1, &raw_events.header);
		xcb_generic_error_t *const select_error = xcb_request_check(connection, select_cookie);
		if (select_error != nullptr)
		{
			reshade::log::message(reshade::log::level::warning, "XInput2 raw event subscription failed root=%#x error=%u.", root, select_error->error_code);
			free(select_error);
			return false;
		}
		xcb_flush(connection);
		wine_input_available = initialize_wine_input_bridge();
		if (wine_input_available)
		{
			const char atom_name[] = "_NET_WM_PID";
			xcb_intern_atom_reply_t *const atom = xcb_intern_atom_reply(connection, xcb_intern_atom(connection, true, sizeof(atom_name) - 1, atom_name), nullptr);
			if (atom != nullptr)
			{
				net_wm_pid_atom = atom->atom;
				free(atom);
			}
		}
		// Focus may have been established before ReShade subscribed to events. X11
		// does not replay the corresponding FocusIn/EnterNotify events to a new client.
		query_initial_focus();

		const bool keyboard_ready = cache_key_translation();
		reshade::log::message(reshade::log::level::info, "X11 input: xcb_window=%#x keyboard=%s pointer=%s cursor_hiding=%s translation=xcb-cached.", window, keyboard_ready ? "yes" : "no", wine_input_available ? "wine-win32u" : "xinput2", wine_set_cursor != nullptr ? "wine-win32u" : (xfixes_cursor_hiding ? "xfixes" : "no"));
#if RESHADE_VERBOSE_LOG
		reshade::log::message(reshade::log::level::debug, "X11 backend ready window=%#x keyboard_focus=%d pointer_focus=%d connection_error=%d.", window, keyboard_focused, pointer_focused, xcb_connection_has_error(connection));
#endif
		return keyboard_ready;
	}
};
