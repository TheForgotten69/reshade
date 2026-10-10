#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-compose.h>

namespace reshade
{
	extern std::atomic<bool> s_keyboard_layout_german;

	unsigned int virtual_key_from_keysym(xkb_keysym_t keysym);
	unsigned int virtual_key_from_evdev_button(uint32_t button);
	unsigned int virtual_key_from_x11_button(uint32_t button);

	void update_keyboard_layout_german(xkb_keymap *keymap, xkb_layout_index_t group);

	class text_composer
	{
	public:
		text_composer() = default;
		~text_composer() { set_table(nullptr); }
		text_composer(const text_composer &) = delete;
		text_composer &operator=(const text_composer &) = delete;

		void load_locale_table();
		void set_table(xkb_compose_table *table);

		std::u32string feed(xkb_keysym_t keysym, uint32_t utf32);
		void reset();

	private:
		xkb_compose_table *_table = nullptr;
		xkb_compose_state *_state = nullptr;
	};
}
