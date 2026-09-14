#pragma once

#include <atomic>
#include <xkbcommon/xkbcommon.h>

namespace reshade
{
	// Windows reports this per-thread via 'GetKeyboardLayout'; there is no equivalent global
	// query on Linux, so the most recently active XKB layout is cached here instead. Only used
	// to pick the correct label for the Home key ("Pos1" on German keyboards).
	extern std::atomic<bool> s_keyboard_layout_german;

	// Maps an XKB keysym to the matching ReShade/Windows virtual-key code. Shared by the Wayland
	// and X11 backends so neither duplicates this mapping.
	unsigned int virtual_key_from_keysym(xkb_keysym_t keysym);

	// Updates 's_keyboard_layout_german' from the active layout of 'group' in 'keymap'. Called by
	// each backend's own keymap/modifier handling.
	void update_keyboard_layout_german(xkb_keymap *keymap, xkb_layout_index_t group);
}
