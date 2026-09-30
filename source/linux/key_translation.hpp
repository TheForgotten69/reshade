#pragma once

#include <atomic>
#include <cstdint>
#include <xkbcommon/xkbcommon.h>

namespace reshade
{
	// Linux has no 'GetKeyboardLayout', so backends cache whether the active XKB layout is German
	// (only used to label the Home key "Pos1").
	extern std::atomic<bool> s_keyboard_layout_german;

	// Translations to ReShade (Windows) virtual-key codes, 0 for keys ReShade does not track.
	unsigned int virtual_key_from_keysym(xkb_keysym_t keysym);
	unsigned int virtual_key_from_evdev_button(uint32_t button);
	unsigned int virtual_key_from_x11_button(uint32_t button);

	void update_keyboard_layout_german(xkb_keymap *keymap, xkb_layout_index_t group);
}
