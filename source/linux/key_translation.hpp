#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-compose.h>

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

	// Applies dead keys and compose sequences to typed text. Without a compose table, text passes through unchanged.
	class text_composer
	{
	public:
		text_composer() = default;
		~text_composer() { set_table(nullptr); }
		text_composer(const text_composer &) = delete;
		text_composer &operator=(const text_composer &) = delete;

		// Loads the compose table of the user's locale, like GTK and Qt do.
		void load_locale_table();
		// Takes ownership of 'table'.
		void set_table(xkb_compose_table *table);

		// Returns the text typed by a key press with 'keysym', which types 'utf32' on its own: nothing while a
		// sequence is pending or when a key cancels it, and the composed text when it completes.
		std::u32string feed(xkb_keysym_t keysym, uint32_t utf32);
		void reset();

	private:
		xkb_compose_table *_table = nullptr;
		xkb_compose_state *_state = nullptr;
	};
}
