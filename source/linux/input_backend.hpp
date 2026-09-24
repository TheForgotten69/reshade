#pragma once

#include "input.hpp"
#include <cstdint>
#include <string>

namespace reshade
{
	// Grants the unit tests access to backend internals.
	struct input_test_access;

	// Base of the Wayland and X11 input backends. Holds the state they share and is the only
	// writer of the 'reshade::input' state they produce.
	class input_backend
	{
	public:
		explicit input_backend(input &owner) : _owner(owner) {}
		virtual ~input_backend() = default;
		input_backend(const input_backend &) = delete;
		input_backend &operator=(const input_backend &) = delete;

		virtual bool initialize() = 0;
		// Consumes the events received since the previous frame.
		virtual void next_frame() = 0;
		virtual const char *name() const = 0;
		// Over a capture layer the host's cursor is hidden, elsewhere the overlay draws a cursor only while the host shows none.
		bool needs_overlay_cursor() const { return is_pointer_in_capture() || is_host_cursor_hidden(); }
		virtual std::string clipboard_text() { return {}; }
		virtual void set_clipboard_text(const char *) {}

		// Whether the overlay currently wants the pointer (see 'input::block_mouse_cursor_warping').
		void set_overlay_active(bool active);
		void set_extent(unsigned int width, unsigned int height);
		unsigned int width() const { return _width; }
		unsigned int height() const { return _height; }
		bool keyboard_focused() const { return _keyboard_focused; }
		bool pointer_focused() const { return _pointer_focused; }

		// A capture region in pixels of the layer that implements it, clipped to that layer.
		struct pixel_rect
		{
			int32_t x, y, width, height;

			bool operator==(const pixel_rect &other) const { return x == other.x && y == other.y && width == other.width && height == other.height; }
		};
		// Converts the normalized capture regions, dropping those that are empty after clipping.
		static std::vector<pixel_rect> to_pixel_rects(const std::vector<input::capture_rect> &regions, unsigned int width, unsigned int height);

	protected:
		static constexpr unsigned int mouse_keys[] = { input::key_button_left, input::key_button_right, input::key_button_middle, input::key_button_xbutton1, input::key_button_xbutton2 };

		virtual void on_overlay_active_changed() {}
		virtual bool is_host_cursor_hidden() const = 0;
		bool overlay_active() const { return _overlay_active; }
		const std::vector<input::capture_rect> &pointer_capture() const { return _owner._pointer_capture; }
		bool is_pointer_in_capture() const;
		bool is_key_down(unsigned int key) const { return _owner.is_key_down(key); }

		// Updates 'key' and keeps the side-agnostic Ctrl/Shift/Alt keys in sync with their sides.
		void set_key(unsigned int key, bool down);
		void set_modifiers(bool ctrl, bool shift, bool alt);
		void add_text(uint32_t utf32);
		void add_wheel_delta(int delta);
		void set_mouse_position(unsigned int x, unsigned int y);
		// Releases every held key (or mouse button) and drops its pending transitions, e.g. on focus loss.
		void release_keyboard();
		void release_pointer();

		input &_owner;
		unsigned int _width = 1;
		unsigned int _height = 1;
		bool _keyboard_focused = false;
		bool _pointer_focused = false;

	private:
		void update_key_state(unsigned int key, bool down);
		void release_keys(bool mouse);

		bool _overlay_active = false;
	};
}
