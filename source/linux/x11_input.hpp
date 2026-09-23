#pragma once

#include "input_backend.hpp"
#include "wine_input_bridge.hpp"
#include <array>
#include <xcb/xcb.h>
#include <xkbcommon/xkbcommon.h>

namespace reshade
{
	// Input for one X11/XWayland Vulkan window. Uses a private XCB connection with non-exclusive
	// XInput2 raw events. Wine hosts expose only an off-screen X11 window for Vulkan, so pointer
	// state and cursor hiding go through Wine's own win32u when available.
	class x11_input final : public input_backend
	{
	public:
		x11_input(input &owner, xcb_window_t window, void *wsi_display, input::wsi_kind wsi_kind);
		~x11_input() override;

		bool initialize() override;
		void next_frame() override;
		const char *name() const override { return "X11"; }

		static uint32_t keysym_to_utf32(xcb_keysym_t keysym);
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

		void update_cursor_policy() override;
		bool cache_key_translations();
		bool is_ancestor_window(xcb_window_t ancestor, xcb_window_t window) const;
		bool contains_window(xcb_window_t window) const;
		void refresh_keyboard_focus();
		bool query_fullscreen() const;
		// Fullscreen and Wine hosts may lock and recenter the pointer, so the overlay cursor then
		// follows raw deltas instead of the recentered pointer position.
		bool uses_relative_motion() const;
		void query_pointer();
		void query_wine_buttons();
		void on_raw_motion(double dx, double dy);
		void set_native_cursor_hidden(bool hidden);

		const xcb_window_t _window;
		void *const _wsi_display;
		const input::wsi_kind _wsi_kind;
		xcb_connection_t *_connection = nullptr;
		xcb_window_t _root = XCB_WINDOW_NONE;
		uint8_t _xinput_opcode = 0;
		xcb_atom_t _wm_state_atom = XCB_ATOM_NONE;
		xcb_atom_t _fullscreen_atom = XCB_ATOM_NONE;
		bool _xfixes_cursor_hiding = false;
		bool _native_cursor_hidden = false;
		bool _fullscreen = false;
		xcb_window_t _keyboard_window = XCB_WINDOW_NONE;
		xcb_window_t _last_observed_focus = XCB_WINDOW_NONE;
		std::array<key_translation, 256> _key_translations = {};
		wine_input_bridge _wine;
		bool _wine_available = false;

		friend struct input_test_access;
	};
}
