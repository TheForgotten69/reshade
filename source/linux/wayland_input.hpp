#pragma once

#include "input.hpp"
#include "dll_log.hpp"
#include "clipboard.hpp"
#include "key_translation.hpp"
#include "window_registry.hpp"
#include "wayland_scale_probe.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <deque>
#include <dlfcn.h>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/input-event-codes.h>
#include <wayland-client.h>
#include "xdg-output-unstable-v1-client-protocol.h"
#include "relative-pointer-unstable-v1-client-protocol.h"

// Owns one Wayland connection's worth of input state for a single ReShade-hooked Vulkan surface:
// registry/seat discovery, keyboard and pointer state, output-scale tracking for pointer coordinate
// mapping, and clipboard integration. See 'dispatch_pending_events' for the event-ingestion
// invariant this depends on, and 'to_framebuffer_pointer_position' for the coordinate-mapping one.
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
	// ImGui renders its own cursor while the overlay is active. Wayland remains an input-only
	// adapter. Once relative events arrive, use them for the whole overlay/focus session so
	// host recenter events cannot overwrite the virtual cursor between render batches.
	bool software_cursor_active = false;
	bool pointer_event_batch_active = false;
	bool absolute_pointer_motion_in_batch = false;
	bool relative_cursor_active = false;
	double absolute_pointer_position[2] = {};
	double virtual_pointer_position[2] = {};
	double relative_pointer_delta[2] = {};
	xkb_context *xkb_context = nullptr;
	xkb_keymap *keymap = nullptr;
	xkb_state *state = nullptr;
	bool keyboard_focused = false;
	wl_surface *keyboard_focus_surface = nullptr;
	bool keyboard_focus_exact = false;
	bool pointer_focused = false;
	bool wine_compatibility = false;
	int32_t scroll_steps = 0;
	bool has_scroll_steps = false;
	double scroll_distance = 0.0;
	unsigned int width = 1;
	unsigned int height = 1;
	// Elements are referenced by address from Wayland listener 'data' pointers once bound, so
	// this must not be a 'std::vector' (whose elements can move on reallocation).
	std::deque<output_info> outputs;
	zxdg_output_manager_v1 *xdg_output_manager = nullptr;
	uint32_t xdg_output_manager_global_name = 0;
	// Best-effort ratio of physical to logical pixels for the connected outputs; NOT applied to
	// pointer coordinates (see 'to_framebuffer_pointer_position' for why), only surfaced through the
	// verbose log as a diagnostic. 0 when no reliable compositor-wide scale exists.
	double output_scale = 0.0;
	wayland_scale_probe surface_scale_probe;
	double last_preferred_surface_scale = 0.0;
	double pointer_scale = 1.0;
	void update_pointer_scale(double preferred)
	{
		if (wine_compatibility || !std::isfinite(preferred) || preferred <= 0.0 || preferred == pointer_scale)
			return;
		const double ratio = preferred / pointer_scale;
		pointer_scale = preferred;
		unsigned int maximum[2];
		max_pointer_position(maximum);
		for (unsigned int axis = 0; axis < 2; ++axis)
		{
			virtual_pointer_position[axis] = std::clamp(virtual_pointer_position[axis] * ratio, 0.0, static_cast<double>(maximum[axis]));
			absolute_pointer_position[axis] = std::clamp(absolute_pointer_position[axis] * ratio, 0.0, static_cast<double>(maximum[axis]));
			if (owner != nullptr)
				owner->_mouse_position[axis] = static_cast<unsigned int>(std::lround(virtual_pointer_position[axis]));
		}
	}

	// Clipboard integration via the core 'wl_data_device' protocol (no per-surface exclusivity,
	// unlike 'wp-fractional-scale-v1', so unlike output scaling this is always safe to use).
	wl_data_device_manager *data_device_manager = nullptr;
	wl_data_device *data_device = nullptr;
	// Most recent serial from a key/button press, required by 'wl_data_device_set_selection'.
	uint32_t last_serial = 0;
	// The selection source when this instance itself set the clipboard text.
	wl_data_source *clipboard_source = nullptr;
	std::string clipboard_text;
	struct data_offer_info
	{
		bool has_text = false;
		std::string mime_type;
	};
	// A compositor may announce multiple selection and drag-and-drop offers before identifying
	// their role. Keep every server-created proxy alive and tracked until it is selected or discarded.
	std::unordered_map<wl_data_offer *, data_offer_info> data_offers;
	// The offer backing the current clipboard contents, once confirmed via 'data_device_selection'.
	wl_data_offer *clipboard_offer = nullptr;
	bool clipboard_offer_has_text = false;
	std::string clipboard_offer_mime_type;

	~wayland_input_context()
	{
		// Server-created proxies (notably wl_data_offer) are attached while messages are
		// demarshaled, before their listener callback runs. Dispatch callbacks already queued
		// for this context so every such proxy is accounted for before destroying the queue.
		if (display != nullptr && queue != nullptr)
			wl_display_dispatch_queue_pending(display, queue);
		surface_scale_probe.reset();
		if (clipboard_source != nullptr)
			wl_data_source_destroy(clipboard_source);
		for (const auto &[offer, info] : data_offers)
			wl_data_offer_destroy(offer);
		if (data_device != nullptr)
			wl_data_device_destroy(data_device);
		if (data_device_manager != nullptr)
			wl_data_device_manager_destroy(data_device_manager);
		for (output_info &info : outputs)
		{
			if (info.xdg_output != nullptr)
				zxdg_output_v1_destroy(info.xdg_output);
			if (info.output != nullptr)
				wl_output_destroy(info.output);
		}
		if (xdg_output_manager != nullptr)
			zxdg_output_manager_v1_destroy(xdg_output_manager);
		if (relative_pointer != nullptr)
			zwp_relative_pointer_v1_destroy(relative_pointer);
		if (relative_pointer_manager != nullptr)
			zwp_relative_pointer_manager_v1_destroy(relative_pointer_manager);
		if (pointer != nullptr)
			wl_pointer_destroy(pointer);
		if (keyboard != nullptr)
			wl_keyboard_destroy(keyboard);
		if (seat != nullptr)
			wl_seat_destroy(seat);
		if (registry != nullptr)
			wl_registry_destroy(registry);
		if (queue != nullptr)
			wl_event_queue_destroy(queue);
		if (state != nullptr)
			xkb_state_unref(state);
		if (keymap != nullptr)
			xkb_keymap_unref(keymap);
		if (xkb_context != nullptr)
			xkb_context_unref(xkb_context);
	}

	// Upper bound on clipboard text kept in memory, in either direction; matches the cap already
	// applied to the synchronous read loop below.
	static constexpr size_t max_clipboard_text_size = 16 * 1024 * 1024;
	// Upper bound on concurrent detached clipboard-send workers (see 'data_source_send'). A peer
	// that never reads its end of the pipe would otherwise let these accumulate for as long as the
	// process runs; beyond the cap, a new send is refused (the requesting peer sees an empty/closed
	// pipe) rather than growing the thread count without limit.
	static constexpr int max_clipboard_send_threads = 4;
	// Heap-allocated (rather than a plain member) so a detached send thread can keep it alive via
	// 'shared_ptr' independently of this 'wayland_input_context', which may be destroyed - and its
	// destructor run on the render thread - before a slow peer finishes reading its transfer.
	std::shared_ptr<std::atomic<int>> clipboard_send_threads_in_flight = std::make_shared<std::atomic<int>>(0);

	// Replaces the current clipboard selection with 'text'. Requires a recent input serial
	// (see 'last_serial'), per Wayland's protection against unsolicited clipboard hijacking.
	void set_clipboard_text(const char *text)
	{
		if (data_device == nullptr)
			return;
		clipboard_text.assign(text, std::min(std::strlen(text), max_clipboard_text_size));
		if (clipboard_source != nullptr)
			wl_data_source_destroy(clipboard_source);
		clipboard_source = wl_data_device_manager_create_data_source(data_device_manager);
		wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(clipboard_source), queue);
		static const wl_data_source_listener listener = {data_source_target, data_source_send, data_source_cancelled, data_source_dnd_drop_performed, data_source_dnd_finished, data_source_action};
		wl_data_source_add_listener(clipboard_source, &listener, this);
		wl_data_source_offer(clipboard_source, "text/plain;charset=utf-8");
		wl_data_source_offer(clipboard_source, "text/plain");
		wl_data_source_offer(clipboard_source, "UTF8_STRING");
		wl_data_device_set_selection(data_device, clipboard_source, last_serial);
	}

	// Reads the current clipboard selection as UTF-8 text, or an empty string if it holds
	// something else (or nothing). ImGui may call this from within the render/present thread's
	// frame update (Ctrl+V), so the wait is bounded far tighter than the second-scale timeouts
	// other Wayland toolkits use for a user-initiated, off-render-thread paste: a well-behaved
	// peer on the same machine answers a pipe read in well under this window, and a peer that
	// does not is treated as having no text rather than stalling a frame.
	std::string get_clipboard_text()
	{
		// Our source is served on this same queue, which cannot dispatch while
		// the synchronous getter waits for a pipe transfer.
		if (clipboard_source != nullptr)
			return clipboard_text;
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
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
		while (result.size() < max_clipboard_text_size)
		{
			pollfd pfd{fds[0], POLLIN, 0};
			const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
			if (remaining.count() <= 0 || poll(&pfd, 1, static_cast<int>(remaining.count())) <= 0)
				break;
			const ssize_t n = read(fds[0], buffer, sizeof(buffer));
			if (n <= 0)
				break;
			result.append(buffer, std::min(static_cast<size_t>(n), max_clipboard_text_size - result.size()));
		}
		close(fds[0]);
		return result;
	}

	// Derives a compositor-wide logical-to-physical pixel ratio from the connected outputs'
	// 'wl_output' pixel mode versus their 'xdg-output' logical size. This is the same workaround
	// toolkits used to detect fractional scaling before 'wp-fractional-scale-v1' existed; it is
	// necessarily an approximation of the actual surface's buffer scale, since a foreign wl_surface
	// cannot be queried directly (see 'to_framebuffer_pointer_position' below) and outputs can run
	// at different scales. 'output_scale' is left at 0 (unknown) whenever outputs disagree, so
	// callers have an explicit signal to fall back on rather than silently using a wrong ratio.
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
	// Best-effort native fallback: assume the host follows its surface's compositor preference.
	// This is NOT the actual buffer/viewport transform (a scale-1 host can ignore the preference).
	// Never use the compositor-wide output scale, or apply this fallback to Wine coordinates.
	double to_framebuffer_pointer_position(double logical, uint32_t axis_extent) const
	{
		return std::clamp(logical * pointer_scale, 0.0, static_cast<double>(axis_extent));
	}
	void clear_keyboard_state()
	{
		auto &events = owner->_key_transitions;
		events.erase(std::remove_if(events.begin(), events.end(), [](const auto &event) { return event.key > input::key_button_xbutton2; }), events.end());
		for (unsigned int key = input::key_button_xbutton2 + 1; key < std::size(owner->_keys); ++key)
			owner->_keys[key] = (owner->_keys[key] & 0x80) != 0 ? 0x08 : 0;
	}
	void clear_pointer_state()
	{
		auto &events = owner->_key_transitions;
		events.erase(std::remove_if(events.begin(), events.end(), [](const auto &event) { return event.key <= input::key_button_xbutton2; }), events.end());
		relative_cursor_active = false;
		absolute_pointer_motion_in_batch = false;
		relative_pointer_delta[0] = relative_pointer_delta[1] = 0.0;
		scroll_steps = 0;
		has_scroll_steps = false;
		scroll_distance = 0.0;
		constexpr unsigned int keys[] = {input::key_button_left, input::key_button_right, input::key_button_middle, input::key_button_xbutton1, input::key_button_xbutton2};
		for (const unsigned int key : keys)
			owner->_keys[key] = (owner->_keys[key] & 0x80) != 0 ? 0x08 : 0;
	}
	bool accepts_focus(wl_surface *focused_surface, const char *device)
	{
		const bool exact_match = focused_surface == surface;
		bool single_surface_fallback = false;
		if (!exact_match)
		{
			std::lock_guard<std::mutex> lock(s_wayland_surfaces_mutex);
			unsigned int surface_count = 0;
			for (const auto &[registered_surface, info] : s_wayland_surfaces)
				if (info.display == display && ++surface_count > 1)
					break;
			single_surface_fallback = surface_count == 1;
		}

		const bool compatibility_match = wine_compatibility && single_surface_fallback;
		reshade::log::message(reshade::log::level::info, "Wayland %s enter: focused_surface=%p vulkan_surface=%p exact_match=%d%s.", device, focused_surface, surface, exact_match, compatibility_match ? " fallback=wine-single-surface" : "");
		return exact_match || compatibility_match;
	}

	void set_key(uint32_t key, uint32_t key_state)
	{
		if (state == nullptr)
			return;
		const xkb_keycode_t xkb_key = key + 8;
		const xkb_keysym_t keysym = xkb_state_key_get_one_sym(state, xkb_key);
		const unsigned int virtual_key = virtual_key_from_keysym(keysym);
		if (virtual_key != 0)
		{
			owner->update_key_state(virtual_key, key_state == WL_KEYBOARD_KEY_STATE_PRESSED);
			if (virtual_key == input::key_left_ctrl || virtual_key == input::key_right_ctrl)
				owner->update_key_state(input::key_ctrl, owner->is_key_down(input::key_left_ctrl) || owner->is_key_down(input::key_right_ctrl));
			if (virtual_key == input::key_left_shift || virtual_key == input::key_right_shift)
				owner->update_key_state(input::key_shift, owner->is_key_down(input::key_left_shift) || owner->is_key_down(input::key_right_shift));
			if (virtual_key == input::key_left_alt || virtual_key == input::key_right_alt)
				owner->update_key_state(input::key_alt, owner->is_key_down(input::key_left_alt) || owner->is_key_down(input::key_right_alt));
		}
		if (key_state == WL_KEYBOARD_KEY_STATE_PRESSED)
		{
			const uint32_t utf32 = xkb_state_key_get_utf32(state, xkb_key);
			if (utf32 != 0 && utf32 <= 0x10ffff && (utf32 < 0xd800 || utf32 > 0xdfff))
				owner->_text_input += static_cast<wchar_t>(utf32);
		}
		xkb_state_update_key(state, xkb_key, key_state == WL_KEYBOARD_KEY_STATE_PRESSED ? XKB_KEY_DOWN : XKB_KEY_UP);
	}

	static void registry_global(void *data, wl_registry *registry, uint32_t name, const char *interface, uint32_t version)
	{
		auto *context = static_cast<wayland_input_context *>(data);
		if (!context->wine_compatibility)
			context->surface_scale_probe.bind(registry, name, interface);
		if (std::strcmp(interface, wl_seat_interface.name) == 0 && context->seat == nullptr)
		{
			context->seat = static_cast<wl_seat *>(wl_registry_bind(registry, name, &wl_seat_interface, std::min(version, 5u)));
			context->seat_global_name = name;
			wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(context->seat), context->queue);
			static const wl_seat_listener listener = {seat_capabilities, seat_name};
			wl_seat_add_listener(context->seat, &listener, context);
		}
		else if (std::strcmp(interface, wl_output_interface.name) == 0)
		{
			output_info &info = context->outputs.emplace_back();
			info.global_name = name;
			info.output = static_cast<wl_output *>(wl_registry_bind(registry, name, &wl_output_interface, std::min(version, 2u)));
			wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(info.output), context->queue);
			static const wl_output_listener listener = {output_geometry, output_mode, output_done, output_scale_event, output_name, output_description};
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
			if (context->pointer != nullptr)
			{
				context->relative_pointer = zwp_relative_pointer_manager_v1_get_relative_pointer(context->relative_pointer_manager, context->pointer);
				wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(context->relative_pointer), context->queue);
				static const zwp_relative_pointer_v1_listener relative_listener = {relative_pointer_motion};
				zwp_relative_pointer_v1_add_listener(context->relative_pointer, &relative_listener, context);
			}
		}
	}
	static void registry_global_remove(void *data, wl_registry *, uint32_t name)
	{
		auto *context = static_cast<wayland_input_context *>(data);
		if (context->seat_global_name == name)
		{
			context->clear_keyboard_state();
			context->clear_pointer_state();
			if (context->relative_pointer != nullptr)
			{
				zwp_relative_pointer_v1_destroy(context->relative_pointer);
				context->relative_pointer = nullptr;
			}
			if (context->pointer != nullptr)
			{
				wl_pointer_destroy(context->pointer);
				context->pointer = nullptr;
			}
			if (context->keyboard != nullptr)
			{
				wl_keyboard_destroy(context->keyboard);
				context->keyboard = nullptr;
			}
			if (context->seat != nullptr)
			{
				wl_seat_destroy(context->seat);
				context->seat = nullptr;
			}
			context->keyboard_focused = context->pointer_focused = false;
			context->keyboard_focus_surface = nullptr;
			context->keyboard_focus_exact = false;
			context->seat_global_name = 0;
		}
		if (context->xdg_output_manager_global_name == name)
		{
			for (output_info &info : context->outputs)
				if (info.xdg_output != nullptr)
				{
					zxdg_output_v1_destroy(info.xdg_output);
					info.xdg_output = nullptr;
				}
			zxdg_output_manager_v1_destroy(context->xdg_output_manager);
			context->xdg_output_manager = nullptr;
			context->xdg_output_manager_global_name = 0;
			context->compute_output_scale();
		}
		for (output_info &info : context->outputs)
			if (info.global_name == name)
			{
				if (info.xdg_output != nullptr)
				{
					zxdg_output_v1_destroy(info.xdg_output);
					info.xdg_output = nullptr;
				}
				if (info.output != nullptr)
				{
					wl_output_destroy(info.output);
					info.output = nullptr;
				}
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
			static const wl_keyboard_listener listener = {keyboard_keymap, keyboard_enter, keyboard_leave, keyboard_key, keyboard_modifiers, keyboard_repeat_info};
			wl_keyboard_add_listener(context->keyboard, &listener, context);
		}
		else if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) == 0 && context->keyboard != nullptr)
		{
			context->clear_keyboard_state();
			wl_keyboard_destroy(context->keyboard);
			context->keyboard = nullptr;
			context->keyboard_focused = false;
			context->keyboard_focus_surface = nullptr;
			context->keyboard_focus_exact = false;
		}
		if ((capabilities & WL_SEAT_CAPABILITY_POINTER) != 0 && context->pointer == nullptr)
		{
			context->pointer = wl_seat_get_pointer(seat);
			wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(context->pointer), context->queue);
			static const wl_pointer_listener listener = {pointer_enter, pointer_leave, pointer_motion, pointer_button, pointer_axis, pointer_frame, pointer_axis_source, pointer_axis_stop, pointer_axis_discrete};
			wl_pointer_add_listener(context->pointer, &listener, context);
			if (context->relative_pointer_manager != nullptr)
			{
				context->relative_pointer = zwp_relative_pointer_manager_v1_get_relative_pointer(context->relative_pointer_manager, context->pointer);
				wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(context->relative_pointer), context->queue);
				static const zwp_relative_pointer_v1_listener relative_listener = {relative_pointer_motion};
				zwp_relative_pointer_v1_add_listener(context->relative_pointer, &relative_listener, context);
			}
		}
		else if ((capabilities & WL_SEAT_CAPABILITY_POINTER) == 0 && context->pointer != nullptr)
		{
			context->clear_pointer_state();
			if (context->relative_pointer != nullptr)
			{
				zwp_relative_pointer_v1_destroy(context->relative_pointer);
				context->relative_pointer = nullptr;
			}
			wl_pointer_destroy(context->pointer);
			context->pointer = nullptr;
			context->pointer_focused = false;
			context->refresh_keyboard_focus();
		}
	}
	static void seat_name(void *, wl_seat *, const char *) {}
	static void output_geometry(void *, wl_output *, int32_t, int32_t, int32_t, int32_t, int32_t, const char *, const char *, int32_t) {}
	static void output_mode(void *data, wl_output *, uint32_t flags, int32_t width, int32_t height, int32_t)
	{
		if ((flags & WL_OUTPUT_MODE_CURRENT) != 0)
		{
			auto *info = static_cast<output_info *>(data);
			info->mode_width = width;
			info->mode_height = height;
		}
	}
	static void output_done(void *, wl_output *) {}
	static void output_scale_event(void *, wl_output *, int32_t) {}
	static void output_name(void *, wl_output *, const char *) {}
	static void output_description(void *, wl_output *, const char *) {}
	static void xdg_output_logical_position(void *, zxdg_output_v1 *, int32_t, int32_t) {}
	static void xdg_output_logical_size(void *data, zxdg_output_v1 *, int32_t width, int32_t height)
	{
		auto *info = static_cast<output_info *>(data);
		info->logical_width = width;
		info->logical_height = height;
	}
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
		static const wl_data_offer_listener listener = {data_offer_offer, data_offer_source_actions, data_offer_action};
		wl_data_offer_add_listener(offer, &listener, context);
		context->data_offers.try_emplace(offer);
	}
	static void data_device_enter(void *data, wl_data_device *, uint32_t, wl_surface *, wl_fixed_t, wl_fixed_t, wl_data_offer *offer)
	{
		// Drag-and-drop is not implemented, so release its offer as soon as the compositor
		// identifies it. Selection offers are retained by 'data_device_selection' instead.
		auto *context = static_cast<wayland_input_context *>(data);
		if (offer != nullptr && offer != context->clipboard_offer)
		{
			context->data_offers.erase(offer);
			wl_data_offer_destroy(offer);
		}
	}
	static void data_device_leave(void *, wl_data_device *) {}
	static void data_device_motion(void *, wl_data_device *, uint32_t, wl_fixed_t, wl_fixed_t) {}
	static void data_device_drop(void *, wl_data_device *) {}
	static void data_device_selection(void *data, wl_data_device *, wl_data_offer *offer)
	{
		auto *context = static_cast<wayland_input_context *>(data);
		if (context->clipboard_offer != nullptr && context->clipboard_offer != offer)
		{
			context->data_offers.erase(context->clipboard_offer);
			wl_data_offer_destroy(context->clipboard_offer);
		}
		context->clipboard_offer = offer;
		const auto it = context->data_offers.find(offer);
		context->clipboard_offer_has_text = it != context->data_offers.end() && it->second.has_text;
		context->clipboard_offer_mime_type = context->clipboard_offer_has_text ? it->second.mime_type : std::string();
	}
	static void data_offer_offer(void *data, wl_data_offer *offer, const char *mime_type)
	{
		auto *context = static_cast<wayland_input_context *>(data);
		const auto it = context->data_offers.find(offer);
		if (it == context->data_offers.end())
			return;
		if (std::strcmp(mime_type, "text/plain;charset=utf-8") == 0 || std::strcmp(mime_type, "text/plain") == 0 || std::strcmp(mime_type, "UTF8_STRING") == 0)
		{
			it->second.has_text = true;
			if (it->second.mime_type.empty() || std::strcmp(mime_type, "text/plain;charset=utf-8") == 0)
				it->second.mime_type = mime_type;
		}
	}
	static void data_offer_source_actions(void *, wl_data_offer *, uint32_t) {}
	static void data_offer_action(void *, wl_data_offer *, uint32_t) {}
	static void data_source_target(void *, wl_data_source *, const char *) {}
	// Writing the outgoing selection text happens on a detached thread because 'fd' is a pipe
	// whose reader is another, unrelated process; blocking here would stall the render thread
	// for as long as that process takes to read, which the compositor does not bound. Detaching
	// (rather than joining on context destruction, which would reintroduce that same stall) is
	// safe because the thread only captures the pipe fd, a bounded copy of the clipboard text and
	// a 'shared_ptr' keeping the in-flight counter alive - never a pointer into 'context' or any
	// other Wayland state - so it cannot dangle even if this 'wayland_input_context' is destroyed
	// before the thread finishes. The counter bounds how many such threads may exist at once, so a
	// peer that holds its end of the pipe open indefinitely cannot grow the thread count without limit.
	static void data_source_send(void *data, wl_data_source *, const char *, int32_t fd)
	{
		auto *context = static_cast<wayland_input_context *>(data);
		const std::shared_ptr<std::atomic<int>> in_flight = context->clipboard_send_threads_in_flight;
		if (in_flight->fetch_add(1, std::memory_order_relaxed) >= max_clipboard_send_threads)
		{
			in_flight->fetch_sub(1, std::memory_order_relaxed);
			close(fd);
			return;
		}
		try
		{
			std::thread([in_flight, fd, text = context->clipboard_text]()
			            {
				utils::write_clipboard_text(fd, text);
				in_flight->fetch_sub(1, std::memory_order_relaxed);
			}).detach();
		}
		catch (...)
		{
			in_flight->fetch_sub(1, std::memory_order_relaxed);
			close(fd);
		}
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
	static void keyboard_keymap(void *data, wl_keyboard *, uint32_t format, int fd, uint32_t size)
	{
		auto *context = static_cast<wayland_input_context *>(data);
		if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 || size == 0)
		{
			close(fd);
			return;
		}
		void *const map = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
		close(fd);
		if (map == MAP_FAILED)
			return;
		xkb_keymap *const keymap = xkb_keymap_new_from_string(context->xkb_context, static_cast<const char *>(map), XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
		munmap(map, size);
		if (keymap == nullptr)
			return;
		if (context->state != nullptr)
			xkb_state_unref(context->state);
		if (context->keymap != nullptr)
			xkb_keymap_unref(context->keymap);
		context->keymap = keymap;
		context->state = xkb_state_new(keymap);
		update_keyboard_layout_german(keymap, 0);
	}
	static void keyboard_enter(void *data, wl_keyboard *, uint32_t, wl_surface *surface, wl_array *)
	{
		auto *context = static_cast<wayland_input_context *>(data);
		context->clear_keyboard_state();
		context->keyboard_focus_surface = surface;
		context->keyboard_focus_exact = context->accepts_focus(surface, "keyboard");
		context->refresh_keyboard_focus();
	}
	void refresh_keyboard_focus()
	{
		// Qt can focus its parent while the Vulkan subsurface has pointer focus. Events on this
		// display belong to the host client; require pointer focus as evidence before accepting
		// a different keyboard surface, and drop this association on either focus leaving.
		const bool focused = keyboard_focus_surface != nullptr && (keyboard_focus_exact || pointer_focused);
		if (keyboard_focused && !focused)
			clear_keyboard_state();
		keyboard_focused = focused;
		if (focused)
			sync_keyboard_modifiers();
	}
	void sync_keyboard_modifiers()
	{
		if (!keyboard_focused || state == nullptr)
			return;
		owner->update_key_state(input::key_ctrl, xkb_state_mod_name_is_active(state, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) > 0);
		owner->update_key_state(input::key_shift, xkb_state_mod_name_is_active(state, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE) > 0);
		owner->update_key_state(input::key_alt, xkb_state_mod_name_is_active(state, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE) > 0);
	}
	static void keyboard_leave(void *data, wl_keyboard *, uint32_t, wl_surface *)
	{
		auto *context = static_cast<wayland_input_context *>(data);
		context->clear_keyboard_state();
		context->keyboard_focused = false;
		context->keyboard_focus_surface = nullptr;
		context->keyboard_focus_exact = false;
	}
	static void keyboard_key(void *data, wl_keyboard *, uint32_t serial, uint32_t, uint32_t key, uint32_t state)
	{
		auto *context = static_cast<wayland_input_context *>(data);
		if (context->keyboard_focused)
		{
			context->last_serial = serial;
			context->set_key(key, state);
		}
	}
	static void keyboard_modifiers(void *data, wl_keyboard *, uint32_t, uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group)
	{
		auto *context = static_cast<wayland_input_context *>(data);
		if (context->state != nullptr)
		{
			xkb_state_update_mask(context->state, depressed, latched, locked, 0, 0, group);
			context->sync_keyboard_modifiers();
			update_keyboard_layout_german(context->keymap, group);
		}
	}
	static void keyboard_repeat_info(void *, wl_keyboard *, int32_t, int32_t) {}
	void max_pointer_position(unsigned int position[2]) const
	{
		position[0] = std::max(1u, width);
		position[1] = std::max(1u, height);
	}
	void set_absolute_pointer_position(wl_fixed_t x, wl_fixed_t y)
	{
		if (!pointer_focused || relative_cursor_active)
			return;
		const double logical_x = wl_fixed_to_double(x), logical_y = wl_fixed_to_double(y);
		const double framebuffer_x = to_framebuffer_pointer_position(logical_x, std::max(1u, width));
		const double framebuffer_y = to_framebuffer_pointer_position(logical_y, std::max(1u, height));
#if RESHADE_VERBOSE_LOG
		reshade::log::message(reshade::log::level::debug, "Wayland pointer motion logical=(%.2f, %.2f) fallback_scale=%.3f framebuffer=(%.2f, %.2f).", logical_x, logical_y, pointer_scale, framebuffer_x, framebuffer_y);
#endif
		if (pointer_event_batch_active)
		{
			absolute_pointer_motion_in_batch = true;
			absolute_pointer_position[0] = framebuffer_x;
			absolute_pointer_position[1] = framebuffer_y;
		}
		else
		{
			owner->_mouse_position[0] = static_cast<unsigned int>(framebuffer_x);
			owner->_mouse_position[1] = static_cast<unsigned int>(framebuffer_y);
			virtual_pointer_position[0] = framebuffer_x;
			virtual_pointer_position[1] = framebuffer_y;
		}
	}
	void set_software_cursor_active(bool active)
	{
		if (software_cursor_active == active)
			return;
		software_cursor_active = active;
		relative_cursor_active = false;
		virtual_pointer_position[0] = owner->_mouse_position[0];
		virtual_pointer_position[1] = owner->_mouse_position[1];
		relative_pointer_delta[0] = relative_pointer_delta[1] = 0.0;
	}
	void begin_pointer_event_batch()
	{
		pointer_event_batch_active = true;
		absolute_pointer_motion_in_batch = false;
		relative_pointer_delta[0] = relative_pointer_delta[1] = 0.0;
	}
	void finish_pointer_event_batch()
	{
		if (pointer_focused && relative_cursor_active)
		{
			unsigned int maximum[2];
			max_pointer_position(maximum);
			for (unsigned int axis = 0; axis < 2; ++axis)
			{
				virtual_pointer_position[axis] = std::clamp(virtual_pointer_position[axis] + relative_pointer_delta[axis] * pointer_scale, 0.0, static_cast<double>(maximum[axis]));
				owner->_mouse_position[axis] = static_cast<unsigned int>(std::lround(virtual_pointer_position[axis]));
			}
		}
		else if (pointer_focused && absolute_pointer_motion_in_batch)
		{
			for (unsigned int axis = 0; axis < 2; ++axis)
			{
				virtual_pointer_position[axis] = absolute_pointer_position[axis];
				owner->_mouse_position[axis] = static_cast<unsigned int>(absolute_pointer_position[axis]);
			}
		}
		pointer_event_batch_active = false;
		relative_pointer_delta[0] = relative_pointer_delta[1] = 0.0;
	}
	static void pointer_enter(void *data, wl_pointer *, uint32_t, wl_surface *surface, wl_fixed_t x, wl_fixed_t y)
	{
		auto *context = static_cast<wayland_input_context *>(data);
		context->clear_pointer_state();
		context->pointer_focused = context->accepts_focus(surface, "pointer");
		context->refresh_keyboard_focus();
		context->set_absolute_pointer_position(x, y);
		// Seed the new focus session before a relative event from this same batch arrives.
		if (context->pointer_focused && context->pointer_event_batch_active)
			for (unsigned int axis = 0; axis < 2; ++axis)
			{
				context->virtual_pointer_position[axis] = context->absolute_pointer_position[axis];
				context->owner->_mouse_position[axis] = static_cast<unsigned int>(context->absolute_pointer_position[axis]);
			}
	}
	static void pointer_leave(void *data, wl_pointer *, uint32_t, wl_surface *)
	{
		auto *context = static_cast<wayland_input_context *>(data);
		context->clear_pointer_state();
		context->pointer_focused = false;
		context->refresh_keyboard_focus();
		context->relative_pointer_delta[0] = context->relative_pointer_delta[1] = 0.0;
	}
	static void pointer_motion(void *data, wl_pointer *, uint32_t, wl_fixed_t x, wl_fixed_t y)
	{
		static_cast<wayland_input_context *>(data)->set_absolute_pointer_position(x, y);
	}
	static void pointer_button(void *data, wl_pointer *, uint32_t serial, uint32_t, uint32_t button, uint32_t state)
	{
		auto *context = static_cast<wayland_input_context *>(data);
		if (!context->pointer_focused)
			return;
		context->last_serial = serial;
		unsigned int key = 0;
		if (button == BTN_LEFT)
			key = input::key_button_left;
		else if (button == BTN_RIGHT)
			key = input::key_button_right;
		else if (button == BTN_MIDDLE)
			key = input::key_button_middle;
		else if (button == BTN_SIDE)
			key = input::key_button_xbutton1;
		else if (button == BTN_EXTRA)
			key = input::key_button_xbutton2;
		if (key != 0)
			context->owner->update_key_state(key, state == WL_POINTER_BUTTON_STATE_PRESSED);
	}
	static void pointer_axis(void *data, wl_pointer *pointer, uint32_t, uint32_t axis, wl_fixed_t value)
	{
		auto *context = static_cast<wayland_input_context *>(data);
		if (!context->pointer_focused || axis != WL_POINTER_AXIS_VERTICAL_SCROLL)
			return;
		context->scroll_distance += wl_fixed_to_double(value);
		if (wl_pointer_get_version(pointer) < WL_POINTER_FRAME_SINCE_VERSION)
			pointer_frame(data, pointer);
	}
	static void pointer_frame(void *data, wl_pointer *)
	{
		auto *context = static_cast<wayland_input_context *>(data);
		// Discrete and continuous values describe the same wheel event.
		if (context->has_scroll_steps)
			context->owner->_mouse_wheel_delta -= static_cast<short>(context->scroll_steps);
		else if (context->scroll_distance != 0.0)
			context->owner->_mouse_wheel_delta += context->scroll_distance > 0.0 ? -1 : 1;
		context->scroll_steps = 0;
		context->has_scroll_steps = false;
		context->scroll_distance = 0.0;
	}
	static void pointer_axis_source(void *, wl_pointer *, uint32_t) {}
	static void pointer_axis_stop(void *, wl_pointer *, uint32_t, uint32_t) {}
	static void pointer_axis_discrete(void *data, wl_pointer *, uint32_t axis, int32_t discrete)
	{
		auto *context = static_cast<wayland_input_context *>(data);
		if (!context->pointer_focused || axis != WL_POINTER_AXIS_VERTICAL_SCROLL)
			return;
		context->scroll_steps += discrete;
		context->has_scroll_steps = true;
	}
	static void relative_pointer_motion(void *data, zwp_relative_pointer_v1 *, uint32_t, uint32_t, wl_fixed_t dx, wl_fixed_t dy, wl_fixed_t, wl_fixed_t)
	{
		auto *context = static_cast<wayland_input_context *>(data);
		if (!context->pointer_focused || !context->software_cursor_active)
			return;
		context->relative_cursor_active = true;
		context->relative_pointer_delta[0] += wl_fixed_to_double(dx);
		context->relative_pointer_delta[1] += wl_fixed_to_double(dy);
	}

	bool initialize()
	{
		if (void *const module = dlopen("win32u.so", RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD))
		{
			wine_compatibility = true;
			dlclose(module);
		}
		queue = wl_display_create_queue(display);
		xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
		if (queue == nullptr || xkb_context == nullptr)
			return false;

		// Assign the private queue before the registry proxy is created. Creating the proxy on the
		// default queue and moving it afterwards races other dispatchers on this foreign display.
		void *const display_wrapper = wl_proxy_create_wrapper(display);
		if (display_wrapper == nullptr)
			return false;
		wl_proxy_set_queue(static_cast<wl_proxy *>(display_wrapper), queue);
		registry = wl_display_get_registry(static_cast<wl_display *>(display_wrapper));
		wl_proxy_wrapper_destroy(display_wrapper);
		if (registry == nullptr)
			return false;
		static const wl_registry_listener registry_listener = {registry_global, registry_global_remove};
		wl_registry_add_listener(registry, &registry_listener, this);
		if (wl_display_roundtrip_queue(display, queue) < 0 || seat == nullptr)
			return false;
		if (!wine_compatibility)
			surface_scale_probe.attach(surface);

		if (data_device_manager != nullptr)
		{
			data_device = wl_data_device_manager_get_data_device(data_device_manager, seat);
			wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(data_device), queue);
			static const wl_data_device_listener listener = {data_device_data_offer, data_device_enter, data_device_leave, data_device_motion, data_device_drop, data_device_selection};
			wl_data_device_add_listener(data_device, &listener, this);
		}

		// Outputs and the xdg-output manager are both discovered in the roundtrip above, so
		// only now can per-output xdg-output objects be requested (order of registry globals
		// is unspecified, so this cannot be done from within 'registry_global' itself).
		if (xdg_output_manager != nullptr)
			for (output_info &info : outputs)
			{
				info.xdg_output = zxdg_output_manager_v1_get_xdg_output(xdg_output_manager, info.output);
				wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(info.xdg_output), queue);
				static const zxdg_output_v1_listener listener = {xdg_output_logical_position, xdg_output_logical_size, xdg_output_done, xdg_output_name, xdg_output_description};
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
		update_pointer_scale(surface_scale_probe.preferred());
		reshade::log::message(reshade::log::level::info, "Wayland input: wl_display=%p vulkan_surface=%p seat=%s keyboard=%s pointer=%s xkb_state=%s relative_pointer=%s xdg_output=%s.", display, surface, seat != nullptr ? "yes" : "no", keyboard != nullptr ? "yes" : "no", pointer != nullptr ? "yes" : "no", state != nullptr ? "yes" : "no", relative_pointer != nullptr ? "yes" : "no", xdg_output_manager != nullptr ? "yes" : "no");
#if RESHADE_VERBOSE_LOG
		reshade::log::message(reshade::log::level::debug, "Wayland backend ready surface=%p keyboard_focus=%d pointer_focus=%d data_device=%s.", surface, keyboard_focused, pointer_focused, data_device != nullptr ? "yes" : "no");
#endif
		return seat != nullptr;
	}
	// The display belongs to the host application, which remains its only socket reader. Reading it
	// here can block behind another prepared reader even after a zero-timeout poll. Present therefore
	// only dispatches events that libwayland has already routed to this private queue.
	bool dispatch_pending_events()
	{
		begin_pointer_event_batch();
		const int result = wl_display_dispatch_queue_pending(display, queue);
		const double preferred_scale = surface_scale_probe.preferred();
		if (preferred_scale != last_preferred_surface_scale)
		{
			last_preferred_surface_scale = preferred_scale;
			update_pointer_scale(preferred_scale);
			reshade::log::message(reshade::log::level::info, "Wayland surface %p compositor-preferred scale fallback=%.3f (host viewport mapping is unknown).", surface, pointer_scale);
		}
		finish_pointer_event_batch();
		return result >= 0;
	}
};
