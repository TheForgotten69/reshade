#pragma once

#include <cstdint>

namespace reshade
{
	// Optional adapter around Wine's internal win32u entry points. Keeping this compatibility
	// surface isolated lets the X11 backend use normal protocol paths without depending on Wine.
	class wine_input_bridge
	{
	public:
		struct point
		{
			int32_t x;
			int32_t y;
		};
		struct rect
		{
			int32_t left, top, right, bottom;
		};

		// Whether this process is a Wine host, i.e. has 'win32u.so' loaded.
		static bool is_wine_process();

		bool initialize();
		bool available() const;
		bool is_foreground_process() const;
		bool query_pointer_position(point &position, unsigned int width, unsigned int height, bool &focused) const;
		bool button_down(int virtual_key) const;
		// Like ReShade on Windows, lifts the application's cursor clipping while 'release' is true,
		// including clipping it sets meanwhile, and restores it afterwards. Call once per frame.
		void release_cursor_clip(bool release);

	private:
		using get_cursor_pos_fn = int (*)(point *);
		using get_foreground_window_fn = void *(*)();
		using call_hwnd_param_fn = uintptr_t (*)(void *, uintptr_t, uint32_t);
		using call_hwnd_fn = uintptr_t (*)(void *, uint32_t);
		using get_async_key_state_fn = int16_t (*)(int);
		using clip_cursor_fn = int (*)(const rect *);
		using get_clip_cursor_fn = int (*)(rect *);

		get_cursor_pos_fn _get_cursor_pos = nullptr;
		get_foreground_window_fn _get_foreground_window = nullptr;
		call_hwnd_param_fn _call_hwnd_param = nullptr;
		call_hwnd_fn _call_hwnd = nullptr;
		get_async_key_state_fn _get_async_key_state = nullptr;
		clip_cursor_fn _clip_cursor = nullptr;
		get_clip_cursor_fn _get_clip_cursor = nullptr;
		bool _clip_released = false;
		rect _application_clip = {};
		rect _released_clip = {};
	};
}
