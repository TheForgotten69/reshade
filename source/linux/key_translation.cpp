#include "key_translation.hpp"
#include "input.hpp"
#include <cstring>
#include <linux/input-event-codes.h>

namespace reshade
{
	std::atomic<bool> s_keyboard_layout_german = false;

	unsigned int virtual_key_from_keysym(xkb_keysym_t keysym)
	{
		if (keysym >= XKB_KEY_a && keysym <= XKB_KEY_z)
			return 'A' + static_cast<unsigned int>(keysym - XKB_KEY_a);
		if (keysym >= XKB_KEY_A && keysym <= XKB_KEY_Z)
			return 'A' + static_cast<unsigned int>(keysym - XKB_KEY_A);
		if (keysym >= XKB_KEY_0 && keysym <= XKB_KEY_9)
			return '0' + static_cast<unsigned int>(keysym - XKB_KEY_0);
		switch (keysym)
		{
		case XKB_KEY_BackSpace:
			return reshade::input::key_backspace;
		case XKB_KEY_Tab:
			return reshade::input::key_tab;
		case XKB_KEY_Return:
			return reshade::input::key_return;
		case XKB_KEY_Escape:
			return reshade::input::key_escape;
		case XKB_KEY_space:
			return reshade::input::key_space;
		case XKB_KEY_Prior:
			return reshade::input::key_page_up;
		case XKB_KEY_Next:
			return reshade::input::key_page_down;
		case XKB_KEY_End:
			return reshade::input::key_end;
		case XKB_KEY_Home:
		case XKB_KEY_KP_Home:
			return reshade::input::key_home;
		case XKB_KEY_Left:
			return reshade::input::key_left;
		case XKB_KEY_Up:
			return reshade::input::key_up;
		case XKB_KEY_Right:
			return reshade::input::key_right;
		case XKB_KEY_Down:
			return reshade::input::key_down;
		case XKB_KEY_Insert:
			return reshade::input::key_insert;
		case XKB_KEY_Delete:
			return reshade::input::key_delete;
		case XKB_KEY_F1:
			return reshade::input::key_f1;
		case XKB_KEY_F2:
			return reshade::input::key_f2;
		case XKB_KEY_F3:
			return reshade::input::key_f3;
		case XKB_KEY_F4:
			return reshade::input::key_f4;
		case XKB_KEY_F5:
			return reshade::input::key_f5;
		case XKB_KEY_F6:
			return reshade::input::key_f6;
		case XKB_KEY_F7:
			return reshade::input::key_f7;
		case XKB_KEY_F8:
			return reshade::input::key_f8;
		case XKB_KEY_F9:
			return reshade::input::key_f9;
		case XKB_KEY_F10:
			return reshade::input::key_f10;
		case XKB_KEY_F11:
			return reshade::input::key_f11;
		case XKB_KEY_F12:
			return reshade::input::key_f12;
		case XKB_KEY_Control_L:
			return reshade::input::key_left_ctrl;
		case XKB_KEY_Control_R:
			return reshade::input::key_right_ctrl;
		case XKB_KEY_Shift_L:
			return reshade::input::key_left_shift;
		case XKB_KEY_Shift_R:
			return reshade::input::key_right_shift;
		case XKB_KEY_Alt_L:
			return reshade::input::key_left_alt;
		case XKB_KEY_Alt_R:
			return reshade::input::key_right_alt;
		case XKB_KEY_Super_L:
			return reshade::input::key_left_windows;
		case XKB_KEY_Super_R:
			return reshade::input::key_right_windows;
		case XKB_KEY_Menu:
			return reshade::input::key_application;
		case XKB_KEY_comma:
			return reshade::input::key_comma;
		case XKB_KEY_minus:
			return reshade::input::key_minus;
		case XKB_KEY_period:
			return reshade::input::key_period;
		case XKB_KEY_slash:
			return reshade::input::key_slash;
		case XKB_KEY_semicolon:
			return reshade::input::key_semicolon;
		case XKB_KEY_equal:
			return reshade::input::key_plus;
		case XKB_KEY_bracketleft:
			return reshade::input::key_left_bracket;
		case XKB_KEY_backslash:
			return reshade::input::key_backslash;
		case XKB_KEY_bracketright:
			return reshade::input::key_right_bracket;
		case XKB_KEY_apostrophe:
			return reshade::input::key_apostrophe;
		case XKB_KEY_grave:
			return reshade::input::key_grave_accent;
		default:
			return 0;
		}
	}

	unsigned int virtual_key_from_evdev_button(uint32_t button)
	{
		switch (button)
		{
		case BTN_LEFT:
			return reshade::input::key_button_left;
		case BTN_RIGHT:
			return reshade::input::key_button_right;
		case BTN_MIDDLE:
			return reshade::input::key_button_middle;
		case BTN_SIDE:
			return reshade::input::key_button_xbutton1;
		case BTN_EXTRA:
			return reshade::input::key_button_xbutton2;
		default:
			return 0;
		}
	}

	unsigned int virtual_key_from_x11_button(uint32_t button)
	{
		switch (button)
		{
		case 1:
			return reshade::input::key_button_left;
		case 2:
			return reshade::input::key_button_middle;
		case 3:
			return reshade::input::key_button_right;
		case 8:
			return reshade::input::key_button_xbutton1;
		case 9:
			return reshade::input::key_button_xbutton2;
		default:
			return 0;
		}
	}

	void update_keyboard_layout_german(xkb_keymap *keymap, xkb_layout_index_t group)
	{
		if (keymap == nullptr || group >= xkb_keymap_num_layouts(keymap))
			return;
		const char *const name = xkb_keymap_layout_get_name(keymap, group);
		s_keyboard_layout_german = name != nullptr && std::strstr(name, "German") != nullptr;
	}
}
