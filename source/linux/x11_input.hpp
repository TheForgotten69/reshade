#pragma once

#include "input_backend.hpp"
#include "wine_input_bridge.hpp"
#include <array>
#include <vector>
#include <xcb/xcb.h>
#include <xkbcommon/xkbcommon.h>

namespace reshade
{
	// Input for one X11/XWayland Vulkan window. Uses a private XCB connection with non-exclusive
	// XInput2 raw events, and an input-only child window on top of the host's one to take pointer
	// input away from it while the overlay captures input. Wine hosts report the pointer position
	// through Wine's own win32u when available.
	class x11_input final : public input_backend
	{
	public:
		x11_input(input &owner, xcb_window_t window, void *wsi_display, input::wsi_kind wsi_kind);
		~x11_input() override;

		bool initialize() override;
		void next_frame() override;
		const char *name() const override { return "X11"; }

		static uint32_t keysym_to_utf32(xcb_keysym_t keysym);
		// Hidden cursors are fully transparent images (Wine and most toolkits use a blank 1x1 one).
		static bool is_cursor_image_visible(const uint32_t *argb_pixels, size_t count);
		// The window whose keyboard focus counts as ours, or 'XCB_WINDOW_NONE'.
		static xcb_window_t select_keyboard_window(xcb_window_t surface_window, xcb_window_t focused_window, bool surface_related, bool wine_related);

		void on_raw_key(xcb_keycode_t keycode, bool pressed);
		void on_raw_button(uint32_t button, bool pressed);

	private:
		struct key_translation
		{
			xcb_keysym_t keysym = XKB_KEY_NoSymbol;
			uint32_t utf32 = 0;
			uint32_t shifted_utf32 = 0;
		};

		bool is_host_cursor_hidden() const override { return !_host_cursor_visible; }
		bool cache_key_translations();
		bool is_ancestor_window(xcb_window_t ancestor, xcb_window_t window) const;
		bool contains_window(xcb_window_t window) const;
		void refresh_keyboard_focus();
		// Hosts that hide their cursor typically also recenter the pointer, so the overlay cursor
		// then follows raw deltas instead of the pointer position.
		bool uses_relative_motion() const;
		void query_pointer();
		void query_wine_buttons();
		void on_raw_motion(double dx, double dy);
		void update_host_cursor_visibility();
		void update_capture();
		void update_keyboard_grab();
		// The window the host shows its image in, which the capture layer has to cover.
		xcb_window_t find_visible_window() const;
		bool create_capture_window(xcb_window_t parent);
		void apply_capture_shape();

		const xcb_window_t _window;
		void *const _wsi_display;
		const input::wsi_kind _wsi_kind;
		xcb_connection_t *_connection = nullptr;
		xcb_window_t _root = XCB_WINDOW_NONE;
		xcb_atom_t _wm_pid_atom = XCB_ATOM_NONE;
		uint8_t _xinput_opcode = 0;
		// First event code of XFixes, 0 when cursor changes are not tracked.
		uint8_t _xfixes_first_event = 0;
		bool _host_cursor_visible = true;
		bool _shape_available = false;
		xcb_window_t _capture_window = XCB_WINDOW_NONE;
		xcb_window_t _capture_parent = XCB_WINDOW_NONE;
		bool _capture_mapped = false;
		unsigned int _capture_size[2] = {};
		std::vector<input::capture_rect> _capture_shape;
		bool _keyboard_grabbed = false;
		unsigned int _keyboard_grab_retry_delay = 0;
		xcb_window_t _keyboard_window = XCB_WINDOW_NONE;
		xcb_window_t _last_observed_focus = XCB_WINDOW_NONE;
		std::array<key_translation, 256> _key_translations = {};
		wine_input_bridge _wine;
		bool _wine_available = false;

		friend struct input_test_access;
	};
}
