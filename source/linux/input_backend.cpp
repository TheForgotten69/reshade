#include "input_backend.hpp"
#include <algorithm>
#include <iterator>

void reshade::input_backend::set_overlay_active(bool active)
{
	if (active == _overlay_active)
		return;

	_overlay_active = active;
	on_overlay_active_changed();
}

void reshade::input_backend::set_extent(unsigned int width, unsigned int height)
{
	_width = std::max(1u, width);
	_height = std::max(1u, height);
}

bool reshade::input_backend::is_pointer_in_capture() const
{
	if (!_pointer_focused)
		return false;

	const float x = static_cast<float>(_owner._mouse_position[0]) / _width;
	const float y = static_cast<float>(_owner._mouse_position[1]) / _height;
	return std::any_of(pointer_capture().begin(), pointer_capture().end(),
		[x, y](const input::capture_rect &rect) { return x >= rect.x && y >= rect.y && x < rect.x + rect.width && y < rect.y + rect.height; });
}

void reshade::input_backend::set_key(unsigned int key, bool down)
{
	update_key_state(key, down);

	constexpr unsigned int modifiers[][3] = {
		{ input::key_ctrl, input::key_left_ctrl, input::key_right_ctrl },
		{ input::key_shift, input::key_left_shift, input::key_right_shift },
		{ input::key_alt, input::key_left_alt, input::key_right_alt },
	};
	for (const auto &[any_side, left, right] : modifiers)
		if (key == left || key == right)
			update_key_state(any_side, is_key_down(left) || is_key_down(right));
}

void reshade::input_backend::set_modifiers(bool ctrl, bool shift, bool alt)
{
	update_key_state(input::key_ctrl, ctrl);
	update_key_state(input::key_shift, shift);
	update_key_state(input::key_alt, alt);
}

void reshade::input_backend::add_text(uint32_t utf32)
{
	if (utf32 != 0 && utf32 <= 0x10FFFF && (utf32 < 0xD800 || utf32 > 0xDFFF))
		_owner._text_input += static_cast<wchar_t>(utf32);
}

void reshade::input_backend::add_wheel_delta(int delta)
{
	_owner._mouse_wheel_delta = static_cast<short>(_owner._mouse_wheel_delta + delta);
}

void reshade::input_backend::set_mouse_position(unsigned int x, unsigned int y)
{
	_owner._mouse_position[0] = std::min(x, _width);
	_owner._mouse_position[1] = std::min(y, _height);
}

void reshade::input_backend::release_keyboard()
{
	release_keys(false);
}

void reshade::input_backend::release_pointer()
{
	release_keys(true);
}

void reshade::input_backend::update_key_state(unsigned int key, bool down)
{
	uint8_t &state = _owner._keys[key];
	if (((state & 0x80) != 0) == down)
		return;

	_owner._key_transitions.push_back({ key, down });
	if (down)
		_owner._key_press_modifiers[key] = (is_key_down(input::key_ctrl) ? 1 : 0) | (is_key_down(input::key_shift) ? 2 : 0) | (is_key_down(input::key_alt) ? 4 : 0);
	state = (state & (input::key_pressed_in_frame | input::key_released_in_frame)) | (down ? 0x88 | input::key_pressed_in_frame : 0x08 | input::key_released_in_frame);
}

void reshade::input_backend::release_keys(bool mouse)
{
	const auto is_mouse_key = [](unsigned int key) { return key <= input::key_button_xbutton2; };

	auto &transitions = _owner._key_transitions;
	transitions.erase(std::remove_if(transitions.begin(), transitions.end(),
		[&](const input::key_transition &transition) { return is_mouse_key(transition.key) == mouse; }), transitions.end());

	for (unsigned int key = 0; key < std::size(_owner._keys); ++key)
		if (is_mouse_key(key) == mouse)
			_owner._keys[key] = (_owner._keys[key] & 0x80) != 0 ? 0x08 : 0;
}
