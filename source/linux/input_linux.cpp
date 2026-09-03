#include "input.hpp"
#include "dll_log.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include "xdg-output-unstable-v1-client-protocol.h"
#include "relative-pointer-unstable-v1-client-protocol.h"

namespace
{
	// Windows reports this per-thread via 'GetKeyboardLayout'; there is no equivalent global
	// query on Linux, so the most recently active XKB layout is cached here instead. Only used
	// to pick the correct label for the Home key ("Pos1" on German keyboards).
	std::atomic<bool> s_keyboard_layout_german = false;

	std::mutex s_wayland_surfaces_mutex;
	struct wayland_surface_info
	{
		wl_display *display;
		unsigned int width;
		unsigned int height;
		std::weak_ptr<reshade::input> input_instance;
	};
	std::unordered_map<void *, wayland_surface_info> s_wayland_surfaces;

	unsigned int virtual_key_from_keysym(xkb_keysym_t keysym)
	{
		if (keysym >= XKB_KEY_a && keysym <= XKB_KEY_z) return 'A' + static_cast<unsigned int>(keysym - XKB_KEY_a);
		if (keysym >= XKB_KEY_A && keysym <= XKB_KEY_Z) return 'A' + static_cast<unsigned int>(keysym - XKB_KEY_A);
		if (keysym >= XKB_KEY_0 && keysym <= XKB_KEY_9) return '0' + static_cast<unsigned int>(keysym - XKB_KEY_0);
		switch (keysym)
		{
		case XKB_KEY_BackSpace: return reshade::input::key_backspace; case XKB_KEY_Tab: return reshade::input::key_tab;
		case XKB_KEY_Return: return reshade::input::key_return; case XKB_KEY_Escape: return reshade::input::key_escape;
		case XKB_KEY_space: return reshade::input::key_space; case XKB_KEY_Prior: return reshade::input::key_page_up;
		case XKB_KEY_Next: return reshade::input::key_page_down; case XKB_KEY_End: return reshade::input::key_end;
		case XKB_KEY_Home: case XKB_KEY_KP_Home: return reshade::input::key_home; case XKB_KEY_Left: return reshade::input::key_left;
		case XKB_KEY_Up: return reshade::input::key_up; case XKB_KEY_Right: return reshade::input::key_right;
		case XKB_KEY_Down: return reshade::input::key_down; case XKB_KEY_Insert: return reshade::input::key_insert;
		case XKB_KEY_Delete: return reshade::input::key_delete; case XKB_KEY_F1: return reshade::input::key_f1;
		case XKB_KEY_F2: return reshade::input::key_f2; case XKB_KEY_F3: return reshade::input::key_f3;
		case XKB_KEY_F4: return reshade::input::key_f4; case XKB_KEY_F5: return reshade::input::key_f5;
		case XKB_KEY_F6: return reshade::input::key_f6; case XKB_KEY_F7: return reshade::input::key_f7;
		case XKB_KEY_F8: return reshade::input::key_f8; case XKB_KEY_F9: return reshade::input::key_f9;
		case XKB_KEY_F10: return reshade::input::key_f10; case XKB_KEY_F11: return reshade::input::key_f11;
		case XKB_KEY_F12: return reshade::input::key_f12; case XKB_KEY_Control_L: return reshade::input::key_left_ctrl;
		case XKB_KEY_Control_R: return reshade::input::key_right_ctrl; case XKB_KEY_Shift_L: return reshade::input::key_left_shift;
		case XKB_KEY_Shift_R: return reshade::input::key_right_shift; case XKB_KEY_Alt_L: return reshade::input::key_left_alt;
		case XKB_KEY_Alt_R: return reshade::input::key_right_alt; case XKB_KEY_Super_L: return reshade::input::key_left_windows;
		case XKB_KEY_Super_R: return reshade::input::key_right_windows; case XKB_KEY_Menu: return reshade::input::key_application;
		case XKB_KEY_comma: return reshade::input::key_comma; case XKB_KEY_minus: return reshade::input::key_minus;
		case XKB_KEY_period: return reshade::input::key_period; case XKB_KEY_slash: return reshade::input::key_slash;
		case XKB_KEY_semicolon: return reshade::input::key_semicolon; case XKB_KEY_equal: return reshade::input::key_plus;
		case XKB_KEY_bracketleft: return reshade::input::key_left_bracket; case XKB_KEY_backslash: return reshade::input::key_backslash;
		case XKB_KEY_bracketright: return reshade::input::key_right_bracket; case XKB_KEY_apostrophe: return reshade::input::key_apostrophe;
		case XKB_KEY_grave: return reshade::input::key_grave_accent; default: return 0;
		}
	}
}

struct reshade::wayland_input_context
{
	// Tracks a single Wayland output's physical (buffer) size against its logical (post-scale)
	// size, so that pointer coordinates (reported by the compositor in logical units) can be
	// converted to the swapchain's physical pixel space when fractional display scaling is active.
	struct output_info
	{
		uint32_t global_name = 0;
		wl_output *output = nullptr;
		zxdg_output_v1 *xdg_output = nullptr;
		int32_t mode_width = 0, mode_height = 0;
		int32_t logical_width = 0, logical_height = 0;
	};

	input *owner = nullptr;
	wl_display *display = nullptr;
	wl_surface *surface = nullptr;
	wl_event_queue *queue = nullptr;
	wl_registry *registry = nullptr;
	wl_seat *seat = nullptr;
	uint32_t seat_global_name = 0;
	wl_keyboard *keyboard = nullptr;
	wl_pointer *pointer = nullptr;
	zwp_relative_pointer_manager_v1 *relative_pointer_manager = nullptr;
	zwp_relative_pointer_v1 *relative_pointer = nullptr;
	xkb_context *xkb_context = nullptr;
	xkb_keymap *keymap = nullptr;
	xkb_state *state = nullptr;
	bool keyboard_focused = false;
	bool pointer_focused = false;
	uint64_t absolute_motion_count = 0;
	uint64_t relative_motion_count = 0;
	unsigned int width = 1;
	unsigned int height = 1;
	// Elements are referenced by address from Wayland listener 'data' pointers once bound, so
	// this must not be a 'std::vector' (whose elements can move on reallocation).
	std::deque<output_info> outputs;
	zxdg_output_manager_v1 *xdg_output_manager = nullptr;
	uint32_t xdg_output_manager_global_name = 0;
	// Ratio of physical to logical output pixels; 0 when it could not be determined.
	double output_scale = 0.0;

	// Clipboard integration via the core 'wl_data_device' protocol (no per-surface exclusivity,
	// unlike 'wp-fractional-scale-v1', so unlike output scaling this is always safe to use).
	wl_data_device_manager *data_device_manager = nullptr;
	wl_data_device *data_device = nullptr;
	// Most recent serial from a key/button press, required by 'wl_data_device_set_selection'.
	uint32_t last_serial = 0;
	// The selection source when this instance itself set the clipboard text.
	wl_data_source *clipboard_source = nullptr;
	std::string clipboard_text;
	// The offer most recently announced by the compositor, and whether it exposes a plain text
	// MIME type, tracked separately until it is confirmed as the active selection (see comment
	// on 'data_device_data_offer').
	wl_data_offer *pending_offer = nullptr;
	bool pending_offer_has_text = false;
	std::string pending_offer_mime_type;
	// The offer backing the current clipboard contents, once confirmed via 'data_device_selection'.
	wl_data_offer *clipboard_offer = nullptr;
	bool clipboard_offer_has_text = false;
	std::string clipboard_offer_mime_type;

	~wayland_input_context()
	{
		if (clipboard_source != nullptr) wl_data_source_destroy(clipboard_source);
		if (clipboard_offer != nullptr) wl_data_offer_destroy(clipboard_offer);
		if (pending_offer != nullptr && pending_offer != clipboard_offer) wl_data_offer_destroy(pending_offer);
		if (data_device != nullptr) wl_data_device_destroy(data_device);
		if (data_device_manager != nullptr) wl_data_device_manager_destroy(data_device_manager);
		for (output_info &info : outputs)
		{
			if (info.xdg_output != nullptr) zxdg_output_v1_destroy(info.xdg_output);
			if (info.output != nullptr) wl_output_destroy(info.output);
		}
		if (xdg_output_manager != nullptr) zxdg_output_manager_v1_destroy(xdg_output_manager);
		if (relative_pointer != nullptr) zwp_relative_pointer_v1_destroy(relative_pointer);
		if (relative_pointer_manager != nullptr) zwp_relative_pointer_manager_v1_destroy(relative_pointer_manager);
		if (pointer != nullptr) wl_pointer_destroy(pointer); if (keyboard != nullptr) wl_keyboard_destroy(keyboard);
		if (seat != nullptr) wl_seat_destroy(seat); if (registry != nullptr) wl_registry_destroy(registry);
		if (queue != nullptr) wl_event_queue_destroy(queue); if (state != nullptr) xkb_state_unref(state);
		if (keymap != nullptr) xkb_keymap_unref(keymap); if (xkb_context != nullptr) xkb_context_unref(xkb_context);
	}

	// Replaces the current clipboard selection with 'text'. Requires a recent input serial
	// (see 'last_serial'), per Wayland's protection against unsolicited clipboard hijacking.
	void set_clipboard_text(const char *text)
	{
		if (data_device == nullptr)
			return;
		clipboard_text = text;
		if (clipboard_source != nullptr) wl_data_source_destroy(clipboard_source);
		clipboard_source = wl_data_device_manager_create_data_source(data_device_manager);
		wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(clipboard_source), queue);
		static const wl_data_source_listener listener = { data_source_target, data_source_send, data_source_cancelled, data_source_dnd_drop_performed, data_source_dnd_finished, data_source_action };
		wl_data_source_add_listener(clipboard_source, &listener, this);
		wl_data_source_offer(clipboard_source, "text/plain;charset=utf-8");
		wl_data_source_offer(clipboard_source, "text/plain");
		wl_data_source_offer(clipboard_source, "UTF8_STRING");
		wl_data_device_set_selection(data_device, clipboard_source, last_serial);
	}

	// Reads the current clipboard selection as UTF-8 text, or an empty string if it holds
	// something else (or nothing). Blocks briefly (bounded by a 1 second timeout) waiting for
	// the offering client to write the data, matching how other Wayland toolkits (GTK, Qt)
	// implement synchronous clipboard paste.
	std::string get_clipboard_text()
	{
		if (clipboard_offer == nullptr || !clipboard_offer_has_text)
			return {};
		int fds[2];
		if (pipe(fds) != 0)
			return {};
		wl_data_offer_receive(clipboard_offer, clipboard_offer_mime_type.c_str(), fds[1]);
		close(fds[1]);
		wl_display_flush(display);
		std::string result;
		char buffer[4096];
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
		while (result.size() < 16 * 1024 * 1024)
		{
			pollfd pfd { fds[0], POLLIN, 0 };
			const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
			if (remaining.count() <= 0 || poll(&pfd, 1, static_cast<int>(remaining.count())) <= 0)
				break;
			const ssize_t n = read(fds[0], buffer, sizeof(buffer));
			if (n <= 0)
				break;
			result.append(buffer, std::min(static_cast<size_t>(n), 16 * 1024 * 1024 - result.size()));
		}
		close(fds[0]);
		return result;
	}

	// Determines the output scale from 'xdg-output' geometry when every connected output agrees
	// on it; otherwise leaves 'output_scale' at 0 so 'observed_max' is used instead, since there
	// is no way to know which specific output a foreign surface is currently shown on.
	void compute_output_scale()
	{
		output_scale = 0.0;
		double scale = 0.0;
		for (const output_info &info : outputs)
		{
			if (info.output == nullptr)
				continue;
			if (info.mode_width <= 0 || info.mode_height <= 0 || info.logical_width <= 0 || info.logical_height <= 0)
				return;
			const double info_scale = static_cast<double>(info.mode_width) / info.logical_width;
			if (scale == 0.0)
				scale = info_scale;
			else if (std::abs(scale - info_scale) > 0.01)
				return;
		}
		output_scale = scale;
	}
	void clear_keyboard_state()
	{
		for (unsigned int key = input::key_button_xbutton2 + 1; key < std::size(owner->_keys); ++key)
			if ((owner->_keys[key] & 0x80) != 0)
				owner->_keys[key] = 0x08;
	}
	void clear_pointer_state()
	{
		constexpr unsigned int keys[] = { input::key_button_left, input::key_button_right, input::key_button_middle, input::key_button_xbutton1, input::key_button_xbutton2 };
		for (const unsigned int key : keys)
			if ((owner->_keys[key] & 0x80) != 0)
				owner->_keys[key] = 0x08;
	}

	void set_key(uint32_t key, uint32_t key_state)
	{
		if (state == nullptr) return;
		const xkb_keycode_t xkb_key = key + 8;
		const xkb_keysym_t keysym = xkb_state_key_get_one_sym(state, xkb_key);
		const unsigned int virtual_key = virtual_key_from_keysym(keysym);
		if (virtual_key != 0)
		{
			owner->_keys[virtual_key] = key_state == WL_KEYBOARD_KEY_STATE_PRESSED ? 0x88 : 0x08;
			if (virtual_key == input::key_left_ctrl || virtual_key == input::key_right_ctrl) owner->_keys[input::key_ctrl] = (owner->_keys[input::key_left_ctrl] & 0x80) != 0 || (owner->_keys[input::key_right_ctrl] & 0x80) != 0 ? 0x88 : 0x08;
			if (virtual_key == input::key_left_shift || virtual_key == input::key_right_shift) owner->_keys[input::key_shift] = (owner->_keys[input::key_left_shift] & 0x80) != 0 || (owner->_keys[input::key_right_shift] & 0x80) != 0 ? 0x88 : 0x08;
			if (virtual_key == input::key_left_alt || virtual_key == input::key_right_alt) owner->_keys[input::key_alt] = (owner->_keys[input::key_left_alt] & 0x80) != 0 || (owner->_keys[input::key_right_alt] & 0x80) != 0 ? 0x88 : 0x08;
		}
		if (key_state == WL_KEYBOARD_KEY_STATE_PRESSED)
		{
			const uint32_t utf32 = xkb_state_key_get_utf32(state, xkb_key);
			if (utf32 != 0 && utf32 <= 0xffff) owner->_text_input += static_cast<wchar_t>(utf32);
		}
		xkb_state_update_key(state, xkb_key, key_state == WL_KEYBOARD_KEY_STATE_PRESSED ? XKB_KEY_DOWN : XKB_KEY_UP);
	}

	static void registry_global(void *data, wl_registry *registry, uint32_t name, const char *interface, uint32_t version)
	{
		auto *context = static_cast<wayland_input_context *>(data);
		if (std::strcmp(interface, wl_seat_interface.name) == 0 && context->seat == nullptr)
		{
			context->seat = static_cast<wl_seat *>(wl_registry_bind(registry, name, &wl_seat_interface, std::min(version, 5u)));
			context->seat_global_name = name;
			wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(context->seat), context->queue);
			static const wl_seat_listener listener = { seat_capabilities, seat_name };
			wl_seat_add_listener(context->seat, &listener, context);
		}
		else if (std::strcmp(interface, wl_output_interface.name) == 0)
		{
			output_info &info = context->outputs.emplace_back();
			info.global_name = name;
			info.output = static_cast<wl_output *>(wl_registry_bind(registry, name, &wl_output_interface, std::min(version, 2u)));
			wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(info.output), context->queue);
			static const wl_output_listener listener = { output_geometry, output_mode, output_done, output_scale_event, output_name, output_description };
			wl_output_add_listener(info.output, &listener, &info);
		}
		else if (std::strcmp(interface, zxdg_output_manager_v1_interface.name) == 0)
		{
			context->xdg_output_manager = static_cast<zxdg_output_manager_v1 *>(wl_registry_bind(registry, name, &zxdg_output_manager_v1_interface, std::min(version, 3u)));
			context->xdg_output_manager_global_name = name;
			wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(context->xdg_output_manager), context->queue);
		}
		else if (std::strcmp(interface, wl_data_device_manager_interface.name) == 0 && context->data_device_manager == nullptr)
		{
			context->data_device_manager = static_cast<wl_data_device_manager *>(wl_registry_bind(registry, name, &wl_data_device_manager_interface, std::min(version, 3u)));
			wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(context->data_device_manager), context->queue);
		}
		else if (std::strcmp(interface, zwp_relative_pointer_manager_v1_interface.name) == 0 && context->relative_pointer_manager == nullptr)
		{
			context->relative_pointer_manager = static_cast<zwp_relative_pointer_manager_v1 *>(wl_registry_bind(registry, name, &zwp_relative_pointer_manager_v1_interface, std::min(version, 1u)));
			wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(context->relative_pointer_manager), context->queue);
		}
	}
	static void registry_global_remove(void *data, wl_registry *, uint32_t name)
	{
		auto *context = static_cast<wayland_input_context *>(data);
		if (context->seat_global_name == name)
		{
			context->clear_keyboard_state();
			context->clear_pointer_state();
			if (context->pointer != nullptr) { wl_pointer_destroy(context->pointer); context->pointer = nullptr; }
			if (context->keyboard != nullptr) { wl_keyboard_destroy(context->keyboard); context->keyboard = nullptr; }
			if (context->seat != nullptr) { wl_seat_destroy(context->seat); context->seat = nullptr; }
			context->keyboard_focused = context->pointer_focused = false;
			context->seat_global_name = 0;
		}
		if (context->xdg_output_manager_global_name == name)
		{
			for (output_info &info : context->outputs)
				if (info.xdg_output != nullptr) { zxdg_output_v1_destroy(info.xdg_output); info.xdg_output = nullptr; }
			zxdg_output_manager_v1_destroy(context->xdg_output_manager);
			context->xdg_output_manager = nullptr;
			context->xdg_output_manager_global_name = 0;
			context->compute_output_scale();
		}
		for (output_info &info : context->outputs)
			if (info.global_name == name)
			{
				if (info.xdg_output != nullptr) { zxdg_output_v1_destroy(info.xdg_output); info.xdg_output = nullptr; }
				if (info.output != nullptr) { wl_output_destroy(info.output); info.output = nullptr; }
				context->compute_output_scale();
				break;
			}
	}
	static void seat_capabilities(void *data, wl_seat *seat, uint32_t capabilities)
	{
		auto *context = static_cast<wayland_input_context *>(data);
		if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) != 0 && context->keyboard == nullptr)
		{
			context->keyboard = wl_seat_get_keyboard(seat);
			wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(context->keyboard), context->queue);
			static const wl_keyboard_listener listener = { keyboard_keymap, keyboard_enter, keyboard_leave, keyboard_key, keyboard_modifiers, keyboard_repeat_info };
			wl_keyboard_add_listener(context->keyboard, &listener, context);
		}
		else if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) == 0 && context->keyboard != nullptr)
		{
			context->clear_keyboard_state();
			wl_keyboard_destroy(context->keyboard);
			context->keyboard = nullptr;
			context->keyboard_focused = false;
		}
		if ((capabilities & WL_SEAT_CAPABILITY_POINTER) != 0 && context->pointer == nullptr)
		{
			context->pointer = wl_seat_get_pointer(seat);
			wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(context->pointer), context->queue);
			static const wl_pointer_listener listener = { pointer_enter, pointer_leave, pointer_motion, pointer_button, pointer_axis, pointer_frame, pointer_axis_source, pointer_axis_stop, pointer_axis_discrete };
			wl_pointer_add_listener(context->pointer, &listener, context);
		}
		else if ((capabilities & WL_SEAT_CAPABILITY_POINTER) == 0 && context->pointer != nullptr)
		{
			context->clear_pointer_state();
			wl_pointer_destroy(context->pointer);
			context->pointer = nullptr;
			context->pointer_focused = false;
		}
	}
	static void seat_name(void *, wl_seat *, const char *) {}
	static void output_geometry(void *, wl_output *, int32_t, int32_t, int32_t, int32_t, int32_t, const char *, const char *, int32_t) {}
	static void output_mode(void *data, wl_output *, uint32_t flags, int32_t width, int32_t height, int32_t) { if ((flags & WL_OUTPUT_MODE_CURRENT) != 0) { auto *info = static_cast<output_info *>(data); info->mode_width = width; info->mode_height = height; } }
	static void output_done(void *, wl_output *) {}
	static void output_scale_event(void *, wl_output *, int32_t) {}
	static void output_name(void *, wl_output *, const char *) {}
	static void output_description(void *, wl_output *, const char *) {}
	static void xdg_output_logical_position(void *, zxdg_output_v1 *, int32_t, int32_t) {}
	static void xdg_output_logical_size(void *data, zxdg_output_v1 *, int32_t width, int32_t height) { auto *info = static_cast<output_info *>(data); info->logical_width = width; info->logical_height = height; }
	static void xdg_output_done(void *, zxdg_output_v1 *) {}
	static void xdg_output_name(void *, zxdg_output_v1 *, const char *) {}
	static void xdg_output_description(void *, zxdg_output_v1 *, const char *) {}
	// A new offer always arrives via 'data_offer' before it is confirmed to be either a clipboard
	// selection (via 'selection' below) or a drag-and-drop payload (via 'enter', unused here since
	// drag-and-drop is not implemented) - see the Wayland core protocol documentation for
	// 'wl_data_device::data_offer'.
	static void data_device_data_offer(void *data, wl_data_device *, wl_data_offer *offer)
	{
		auto *context = static_cast<wayland_input_context *>(data);
		wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(offer), context->queue);
		static const wl_data_offer_listener listener = { data_offer_offer, data_offer_source_actions, data_offer_action };
		wl_data_offer_add_listener(offer, &listener, context);
		context->pending_offer = offer;
		context->pending_offer_has_text = false;
		context->pending_offer_mime_type.clear();
	}
	static void data_device_enter(void *, wl_data_device *, uint32_t, wl_surface *, wl_fixed_t, wl_fixed_t, wl_data_offer *) {}
	static void data_device_leave(void *, wl_data_device *) {}
	static void data_device_motion(void *, wl_data_device *, uint32_t, wl_fixed_t, wl_fixed_t) {}
	static void data_device_drop(void *, wl_data_device *) {}
	static void data_device_selection(void *data, wl_data_device *, wl_data_offer *offer)
	{
		auto *context = static_cast<wayland_input_context *>(data);
		if (context->clipboard_offer != nullptr && context->clipboard_offer != offer)
			wl_data_offer_destroy(context->clipboard_offer);
		context->clipboard_offer = offer;
		context->clipboard_offer_has_text = offer != nullptr && offer == context->pending_offer && context->pending_offer_has_text;
		context->clipboard_offer_mime_type = context->clipboard_offer_has_text ? std::move(context->pending_offer_mime_type) : std::string();
		context->pending_offer = nullptr;
	}
	static void data_offer_offer(void *data, wl_data_offer *offer, const char *mime_type)
	{
		auto *context = static_cast<wayland_input_context *>(data);
		if (offer != context->pending_offer)
			return;
		if (std::strcmp(mime_type, "text/plain;charset=utf-8") == 0 || std::strcmp(mime_type, "text/plain") == 0 || std::strcmp(mime_type, "UTF8_STRING") == 0)
		{
			context->pending_offer_has_text = true;
			if (context->pending_offer_mime_type.empty() || std::strcmp(mime_type, "text/plain;charset=utf-8") == 0)
				context->pending_offer_mime_type = mime_type;
		}
	}
	static void data_offer_source_actions(void *, wl_data_offer *, uint32_t) {}
	static void data_offer_action(void *, wl_data_offer *, uint32_t) {}
	static void data_source_target(void *, wl_data_source *, const char *) {}
	// Writing the outgoing selection text happens on a detached thread because 'fd' is a pipe
	// whose reader is another, unrelated process; blocking here would stall the render thread
	// for as long as that process takes to read, which the compositor does not bound.
	static void data_source_send(void *data, wl_data_source *, const char *, int32_t fd)
	{
		auto *context = static_cast<wayland_input_context *>(data);
		std::thread([fd, text = context->clipboard_text]() {
			size_t written = 0;
			while (written < text.size())
			{
				const ssize_t n = write(fd, text.data() + written, text.size() - written);
				if (n <= 0)
					break;
				written += static_cast<size_t>(n);
			}
			close(fd);
		}).detach();
	}
	static void data_source_cancelled(void *data, wl_data_source *source)
	{
		auto *context = static_cast<wayland_input_context *>(data);
		if (context->clipboard_source == source)
		{
			wl_data_source_destroy(source);
			context->clipboard_source = nullptr;
		}
	}
	static void data_source_dnd_drop_performed(void *, wl_data_source *) {}
	static void data_source_dnd_finished(void *, wl_data_source *) {}
	static void data_source_action(void *, wl_data_source *, uint32_t) {}
	static void update_keyboard_layout_german(xkb_keymap *keymap, xkb_layout_index_t group)
	{
		if (keymap == nullptr || group >= xkb_keymap_num_layouts(keymap))
			return;
		const char *const name = xkb_keymap_layout_get_name(keymap, group);
		s_keyboard_layout_german = name != nullptr && std::strstr(name, "German") != nullptr;
	}
	static void keyboard_keymap(void *data, wl_keyboard *, uint32_t format, int fd, uint32_t size)
	{
		auto *context = static_cast<wayland_input_context *>(data);
		if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 || size == 0) { close(fd); return; }
		void *const map = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0); close(fd); if (map == MAP_FAILED) return;
		xkb_keymap *const keymap = xkb_keymap_new_from_string(context->xkb_context, static_cast<const char *>(map), XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS); munmap(map, size); if (keymap == nullptr) return;
		if (context->state != nullptr) xkb_state_unref(context->state); if (context->keymap != nullptr) xkb_keymap_unref(context->keymap);
		context->keymap = keymap; context->state = xkb_state_new(keymap);
		update_keyboard_layout_german(keymap, 0);
	}
	static void keyboard_enter(void *data, wl_keyboard *, uint32_t, wl_surface *surface, wl_array *) { auto *context = static_cast<wayland_input_context *>(data); context->keyboard_focused = surface == context->surface; }
	static void keyboard_leave(void *data, wl_keyboard *, uint32_t, wl_surface *) { auto *context = static_cast<wayland_input_context *>(data); context->clear_keyboard_state(); context->keyboard_focused = false; }
	static void keyboard_key(void *data, wl_keyboard *, uint32_t serial, uint32_t, uint32_t key, uint32_t state) { auto *context = static_cast<wayland_input_context *>(data); if (context->keyboard_focused) { context->last_serial = serial; context->set_key(key, state); } }
	static void keyboard_modifiers(void *data, wl_keyboard *, uint32_t, uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group) { auto *context = static_cast<wayland_input_context *>(data); if (context->state != nullptr) { xkb_state_update_mask(context->state, depressed, latched, locked, 0, 0, group); update_keyboard_layout_german(context->keymap, group); } }
	static void keyboard_repeat_info(void *, wl_keyboard *, int32_t, int32_t) {}
	void max_pointer_position(unsigned int position[2]) const
	{
		if (output_scale > 0.0)
		{
			position[0] = std::max(1u, static_cast<unsigned int>(width / output_scale));
			position[1] = std::max(1u, static_cast<unsigned int>(height / output_scale));
		}
		else
		{
			position[0] = width;
			position[1] = height;
		}
	}
	void set_absolute_pointer_position(wl_fixed_t x, wl_fixed_t y)
	{
		++absolute_motion_count;
		if (absolute_motion_count == 1 || absolute_motion_count % 120 == 0)
			log::message(log::level::info, "[DEBUG-wayland-mouse] absolute=%llu relative=%llu focused=%d blocked=%d position=%u,%u event=%d,%d.", static_cast<unsigned long long>(absolute_motion_count), static_cast<unsigned long long>(relative_motion_count), pointer_focused, owner->_block_cursor_warping, owner->_mouse_position[0], owner->_mouse_position[1], wl_fixed_to_int(x), wl_fixed_to_int(y));
		if (!pointer_focused || (owner->_block_cursor_warping && relative_pointer != nullptr))
			return;
		const int px = std::max(0, wl_fixed_to_int(x)), py = std::max(0, wl_fixed_to_int(y));
		owner->_mouse_position[0] = px;
		owner->_mouse_position[1] = py;
	}
	static void pointer_enter(void *data, wl_pointer *, uint32_t, wl_surface *surface, wl_fixed_t x, wl_fixed_t y) { auto *context = static_cast<wayland_input_context *>(data); context->pointer_focused = surface == context->surface; context->set_absolute_pointer_position(x, y); }
	static void pointer_leave(void *data, wl_pointer *, uint32_t, wl_surface *) { auto *context = static_cast<wayland_input_context *>(data); context->clear_pointer_state(); context->pointer_focused = false; }
	static void pointer_motion(void *data, wl_pointer *, uint32_t, wl_fixed_t x, wl_fixed_t y) { static_cast<wayland_input_context *>(data)->set_absolute_pointer_position(x, y); }
	static void pointer_button(void *data, wl_pointer *, uint32_t serial, uint32_t, uint32_t button, uint32_t state) { auto *context = static_cast<wayland_input_context *>(data); if (!context->pointer_focused) return; context->last_serial = serial; unsigned int key = 0; if (button == 0x110) key = input::key_button_left; else if (button == 0x111) key = input::key_button_right; else if (button == 0x112) key = input::key_button_middle; else if (button == 0x113) key = input::key_button_xbutton1; else if (button == 0x114) key = input::key_button_xbutton2; if (key != 0) context->owner->_keys[key] = state == WL_POINTER_BUTTON_STATE_PRESSED ? 0x88 : 0x08; }
	static void pointer_axis(void *data, wl_pointer *, uint32_t, uint32_t axis, wl_fixed_t value) { auto *context = static_cast<wayland_input_context *>(data); if (context->pointer_focused && axis == WL_POINTER_AXIS_VERTICAL_SCROLL && value != 0) context->owner->_mouse_wheel_delta += value > 0 ? -1 : 1; }
	static void pointer_frame(void *, wl_pointer *) {} static void pointer_axis_source(void *, wl_pointer *, uint32_t) {} static void pointer_axis_stop(void *, wl_pointer *, uint32_t, uint32_t) {}
	static void pointer_axis_discrete(void *data, wl_pointer *, uint32_t axis, int32_t discrete) { auto *context = static_cast<wayland_input_context *>(data); if (context->pointer_focused && axis == WL_POINTER_AXIS_VERTICAL_SCROLL) context->owner->_mouse_wheel_delta -= static_cast<short>(discrete); }
	static void relative_pointer_motion(void *data, zwp_relative_pointer_v1 *, uint32_t, uint32_t, wl_fixed_t dx, wl_fixed_t dy, wl_fixed_t, wl_fixed_t)
	{
		auto *context = static_cast<wayland_input_context *>(data);
		++context->relative_motion_count;
		if (context->relative_motion_count == 1 || context->relative_motion_count % 120 == 0)
			log::message(log::level::info, "[DEBUG-wayland-mouse] relative=%llu absolute=%llu focused=%d blocked=%d position=%u,%u delta=%.2f,%.2f.", static_cast<unsigned long long>(context->relative_motion_count), static_cast<unsigned long long>(context->absolute_motion_count), context->pointer_focused, context->owner->_block_cursor_warping, context->owner->_mouse_position[0], context->owner->_mouse_position[1], wl_fixed_to_double(dx), wl_fixed_to_double(dy));
		if (!context->pointer_focused || !context->owner->_block_cursor_warping)
			return;
		unsigned int maximum[2];
		context->max_pointer_position(maximum);
		context->owner->_mouse_position[0] = static_cast<unsigned int>(std::clamp(static_cast<int>(std::lround(context->owner->_mouse_position[0] + wl_fixed_to_double(dx))), 0, static_cast<int>(maximum[0])));
		context->owner->_mouse_position[1] = static_cast<unsigned int>(std::clamp(static_cast<int>(std::lround(context->owner->_mouse_position[1] + wl_fixed_to_double(dy))), 0, static_cast<int>(maximum[1])));
	}

	bool initialize()
	{
		queue = wl_display_create_queue(display); xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS); if (queue == nullptr || xkb_context == nullptr) return false;
		registry = wl_display_get_registry(display); wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(registry), queue);
		static const wl_registry_listener registry_listener = { registry_global, registry_global_remove }; wl_registry_add_listener(registry, &registry_listener, this); if (wl_display_roundtrip_queue(display, queue) < 0 || seat == nullptr) return false;

		if (data_device_manager != nullptr)
		{
			data_device = wl_data_device_manager_get_data_device(data_device_manager, seat);
			wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(data_device), queue);
			static const wl_data_device_listener listener = { data_device_data_offer, data_device_enter, data_device_leave, data_device_motion, data_device_drop, data_device_selection };
			wl_data_device_add_listener(data_device, &listener, this);
		}
		if (relative_pointer_manager != nullptr && pointer != nullptr)
		{
			relative_pointer = zwp_relative_pointer_manager_v1_get_relative_pointer(relative_pointer_manager, pointer);
			wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(relative_pointer), queue);
			static const zwp_relative_pointer_v1_listener listener = { relative_pointer_motion };
			zwp_relative_pointer_v1_add_listener(relative_pointer, &listener, this);
			log::message(log::level::info, "[DEBUG-wayland-mouse] Created relative pointer for surface %p.", surface);
		}
		else
			log::message(log::level::info, "[DEBUG-wayland-mouse] Relative pointer unavailable for surface %p (manager=%p, pointer=%p).", surface, relative_pointer_manager, pointer);

		// Outputs and the xdg-output manager are both discovered in the roundtrip above, so
		// only now can per-output xdg-output objects be requested (order of registry globals
		// is unspecified, so this cannot be done from within 'registry_global' itself).
		if (xdg_output_manager != nullptr)
			for (output_info &info : outputs)
			{
				info.xdg_output = zxdg_output_manager_v1_get_xdg_output(xdg_output_manager, info.output);
				wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(info.xdg_output), queue);
				static const zxdg_output_v1_listener listener = { xdg_output_logical_position, xdg_output_logical_size, xdg_output_done, xdg_output_name, xdg_output_description };
				zxdg_output_v1_add_listener(info.xdg_output, &listener, &info);
			}

		if (wl_display_roundtrip_queue(display, queue) < 0)
			return false;
		// The keyboard and pointer requests are issued from the seat capability callback
		// during the previous roundtrip, so another roundtrip is required to receive their
		// initial events (in particular the keyboard keymap) before returning. This also
		// covers the xdg-output requests issued just above.
		if (wl_display_roundtrip_queue(display, queue) < 0)
			return false;
		compute_output_scale();
		return keyboard != nullptr && state != nullptr;
	}
};

bool reshade::input::is_keyboard_layout_german() { return s_keyboard_layout_german; }
std::shared_ptr<reshade::input> reshade::input::register_window(window_handle window)
{
	wayland_surface_info surface_info;
	{
		std::lock_guard<std::mutex> lock(s_wayland_surfaces_mutex);
		const auto it = s_wayland_surfaces.find(window);
		if (it == s_wayland_surfaces.end() || it->second.display == nullptr)
			return nullptr;
		if (const std::shared_ptr<input> existing = it->second.input_instance.lock())
		{
			existing->_wayland->width = std::max(1u, it->second.width);
			existing->_wayland->height = std::max(1u, it->second.height);
			return existing;
		}
		surface_info = it->second;
	}

	// Wayland round trips may block. Do not hold the surface registry lock while creating
	// the input context, so swapchain destruction and concurrent surface updates can proceed.
	auto result = std::make_shared<input>(window);
	result->_wayland = new wayland_input_context { result.get(), surface_info.display, static_cast<wl_surface *>(window) };
	result->_wayland->width = std::max(1u, surface_info.width);
	result->_wayland->height = std::max(1u, surface_info.height);
	if (!result->_wayland->initialize())
	{
		log::message(log::level::warning, "Failed to initialize Wayland input for surface %p.", window);
		delete result->_wayland;
		result->_wayland = nullptr;
		return nullptr;
	}

	std::lock_guard<std::mutex> lock(s_wayland_surfaces_mutex);
	const auto it = s_wayland_surfaces.find(window);
	if (it == s_wayland_surfaces.end() || it->second.display != surface_info.display)
	{
		// The surface was replaced or destroyed while the initial round trips were pending.
		// Leave it unregistered; a later runtime recreation can retry with the current surface.
		return nullptr;
	}
	if (const std::shared_ptr<input> existing = it->second.input_instance.lock())
	{
		existing->_wayland->width = std::max(1u, it->second.width);
		existing->_wayland->height = std::max(1u, it->second.height);
		return existing;
	}

	log::message(log::level::info, "Initialized Wayland input for surface %p.", window);
	it->second.input_instance = result;
	return result;
}
reshade::input::~input() { delete _wayland; }
void reshade::input::register_wayland_surface(window_handle surface, void *display, unsigned int width, unsigned int height)
{
	std::lock_guard<std::mutex> lock(s_wayland_surfaces_mutex);
	auto [it, inserted] = s_wayland_surfaces.try_emplace(surface);
	it->second.display = static_cast<wl_display *>(display);
	it->second.width = width;
	it->second.height = height;
}
void reshade::input::unregister_wayland_surface(window_handle surface)
{
	std::lock_guard<std::mutex> lock(s_wayland_surfaces_mutex);
	s_wayland_surfaces.erase(surface);
}
const char *reshade::input::get_clipboard_text(void *user_data)
{
	// Owns the string so the returned pointer stays valid for ImGui to read; only ever called
	// from the single-threaded GUI update, so a function-local static is safe here.
	static std::string buffer;
	auto *self = static_cast<input *>(user_data);
	buffer = self != nullptr && self->_wayland != nullptr ? self->_wayland->get_clipboard_text() : std::string();
	return buffer.c_str();
}
void reshade::input::set_clipboard_text(void *user_data, const char *text)
{
	auto *self = static_cast<input *>(user_data);
	if (self != nullptr && self->_wayland != nullptr)
		self->_wayland->set_clipboard_text(text);
}
void reshade::input::register_window_with_raw_input(window_handle, bool, bool) {}
void reshade::input::next_frame()
{
	const std::unique_lock<std::recursive_mutex> lock(_mutex);
	std::copy(std::begin(_keys), std::end(_keys), std::begin(_last_keys));
	for (uint8_t &state : _keys)
		state &= ~0x08;
	std::copy(std::begin(_mouse_position), std::end(_mouse_position), std::begin(_last_mouse_position));
	_mouse_wheel_delta = 0;
	_text_input.clear();
	++_frame_count;
	if (_wayland != nullptr)
		wl_display_dispatch_queue_pending(_wayland->display, _wayland->queue);
}
void reshade::input::max_mouse_position(unsigned int position[2]) const
{
	if (_wayland == nullptr)
	{
		position[0] = position[1] = 1;
		return;
	}
	_wayland->max_pointer_position(position);
}
void reshade::input::block_mouse_cursor_warping(bool enable)
{
	if (_block_cursor_warping != enable)
		log::message(log::level::info, "[DEBUG-wayland-mouse] Cursor-warp blocking changed to %d at position %u,%u (focused=%d, relative=%p).", enable, _mouse_position[0], _mouse_position[1], _wayland != nullptr && _wayland->pointer_focused, _wayland != nullptr ? _wayland->relative_pointer : nullptr);
	_block_cursor_warping = enable;
}
std::shared_ptr<reshade::input_gamepad> reshade::input_gamepad::load() { return {}; }
void reshade::input_gamepad::next_frame() {}
