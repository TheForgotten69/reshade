#include "wine_input_bridge.hpp"

#include <dlfcn.h>

bool reshade::wine_input_bridge::is_wine_process()
{
	void *const module = dlopen("win32u.so", RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD);
	if (module != nullptr)
		dlclose(module);
	return module != nullptr;
}

bool reshade::wine_input_bridge::initialize()
{
	void *const module = dlopen("win32u.so", RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD);
	auto lookup = [module](const char *name) { return dlsym(module != nullptr ? module : RTLD_DEFAULT, name); };
	_get_cursor_pos = reinterpret_cast<get_cursor_pos_fn>(lookup("NtUserGetCursorPos"));
	_get_foreground_window = reinterpret_cast<get_foreground_window_fn>(lookup("NtUserGetForegroundWindow"));
	_call_hwnd_param = reinterpret_cast<call_hwnd_param_fn>(lookup("NtUserCallHwndParam"));
	_call_hwnd = reinterpret_cast<call_hwnd_fn>(lookup("NtUserCallHwnd"));
	_get_async_key_state = reinterpret_cast<get_async_key_state_fn>(lookup("NtUserGetAsyncKeyState"));
	_clip_cursor = reinterpret_cast<clip_cursor_fn>(lookup("NtUserClipCursor"));
	_get_clip_cursor = reinterpret_cast<get_clip_cursor_fn>(lookup("NtUserGetClipCursor"));
	if (module != nullptr)
		dlclose(module);
	return available();
}

bool reshade::wine_input_bridge::is_foreground_process() const
{
	if (_get_foreground_window == nullptr || _call_hwnd == nullptr)
		return false;
	void *const foreground_window = _get_foreground_window();
	// NtUserIsCurrentProcessWindow is Wine internal NtUserCallHwnd operation 16. Keeping the
	// unstable operation number inside this adapter is why generic X11 code does not use it.
	return foreground_window != nullptr && _call_hwnd(foreground_window, 16) != 0;
}

bool reshade::wine_input_bridge::available() const
{
	return _get_cursor_pos != nullptr && _get_foreground_window != nullptr && _call_hwnd_param != nullptr;
}

bool reshade::wine_input_bridge::query_pointer_position(point &position, unsigned int width, unsigned int height, bool &focused) const
{
	if (!available())
		return false;

	void *const foreground_window = _get_foreground_window();
	// NtUserCallHwndParam_ScreenToClient is Wine internal operation 23.
	if (foreground_window == nullptr || !_get_cursor_pos(&position) ||
		_call_hwnd_param(foreground_window, reinterpret_cast<uintptr_t>(&position), 23) == 0)
		return false;

	focused = position.x >= 0 && position.y >= 0 && position.x < static_cast<int32_t>(width) && position.y < static_cast<int32_t>(height);
	return true;
}

bool reshade::wine_input_bridge::button_down(int virtual_key) const
{
	return _get_async_key_state != nullptr && (_get_async_key_state(virtual_key) & 0x8000) != 0;
}

void reshade::wine_input_bridge::release_cursor_clip(bool release)
{
	if (_clip_cursor == nullptr || _get_clip_cursor == nullptr)
		return;

	if (!release)
	{
		if (_clip_released)
			_clip_cursor(&_application_clip);
		_clip_released = false;
		return;
	}

	rect current = {};
	if (!_get_clip_cursor(&current))
		return;
	const auto equal = [](const rect &lhs, const rect &rhs) { return lhs.left == rhs.left && lhs.top == rhs.top && lhs.right == rhs.right && lhs.bottom == rhs.bottom; };
	if (_clip_released && equal(current, _released_clip))
		return;

	// Unclipped reads back as the whole screen, which restores to no clipping as well.
	_application_clip = current;
	_clip_cursor(nullptr);
	_get_clip_cursor(&_released_clip);
	_clip_released = true;
}
