#pragma once

#include "input_backend.hpp"
#include "wayland_clipboard.hpp"
#include "wayland_pointer.hpp"
#include "wayland_overlay_surface.hpp"
#include "wine_input_bridge.hpp"
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

struct zwp_relative_pointer_manager_v1;
struct zwp_relative_pointer_v1;
struct wp_cursor_shape_manager_v1;
struct wp_cursor_shape_device_v1;

namespace reshade
{
	uint32_t wayland_cursor_shape(int cursor);

	class wayland_input final : public input_backend
	{
	public:
		wayland_input(input &owner, wl_display *display, wl_surface *surface);
		~wayland_input() override;

		bool initialize() override;
		void next_frame() override;
		const char *name() const override { return "Wayland"; }
		std::string clipboard_text() override { return _clipboard != nullptr ? _clipboard->text() : std::string(); }
		void set_clipboard_text(const char *text) override { if (_clipboard != nullptr) _clipboard->set_text(text, _last_serial); }
		void set_overlay_cursor(int cursor) override;

		void on_global(wl_registry *registry, uint32_t name, const char *interface, uint32_t version);
		void on_global_remove(uint32_t name);
		void on_seat_capabilities(uint32_t capabilities);
		void on_keymap(uint32_t format, int fd, uint32_t size);
		void on_keyboard_enter(wl_surface *surface);
		void on_keyboard_leave();
		void on_key(uint32_t serial, uint32_t key, uint32_t state);
		void on_modifiers(uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group);
		void on_pointer_enter(uint32_t serial, wl_surface *surface, double x, double y);
		void on_pointer_leave();
		void on_pointer_motion(double x, double y);
		void on_pointer_button(uint32_t serial, uint32_t button, uint32_t state);
		void on_pointer_axis(uint32_t axis, double value);
		void on_pointer_axis_discrete(uint32_t axis, int32_t steps);
		void on_pointer_frame();
		void on_relative_motion(double dx, double dy, uint64_t time);

	private:
		void on_overlay_active_changed() override;
		bool is_host_cursor_hidden() const override { return _pointer.current_mode() == wayland_pointer::mode::software_relative; }
		bool is_capture_layer_shown() const override { return _capturing; }
		bool has_cursor_shapes() const override { return _cursor_shape_device != nullptr; }
		void apply_overlay_cursor();
		void update_pointer_scale();
		void update_capture();
		void publish_pointer();
		void log_pointer_changes();
		bool accepts_focus(wl_surface *focused_surface, const char *device) const;
		void refresh_keyboard_focus();
		void sync_modifiers();
		void bind_pointer_objects();
		void release_keyboard_device();
		void release_pointer_device();

		wl_display *const _display;
		wl_surface *const _surface;
		bool _wine_host = false;
		wine_input_bridge _wine;
		wl_event_queue *_queue = nullptr;
		wl_registry *_registry = nullptr;
		wl_seat *_seat = nullptr;
		uint32_t _seat_name = 0;
		wl_keyboard *_keyboard = nullptr;
		wl_pointer *_pointer_device = nullptr;
		zwp_relative_pointer_manager_v1 *_relative_pointer_manager = nullptr;
		zwp_relative_pointer_v1 *_relative_pointer = nullptr;
		wp_cursor_shape_manager_v1 *_cursor_shape_manager = nullptr;
		uint32_t _cursor_shape_manager_name = 0;
		wp_cursor_shape_device_v1 *_cursor_shape_device = nullptr;
		uint32_t _overlay_enter_serial = 0;
		bool _pointer_on_overlay = false;
		int _overlay_cursor = 0;
		uint32_t _last_serial = 0;

		xkb_context *_xkb_context = nullptr;
		xkb_keymap *_keymap = nullptr;
		xkb_state *_xkb_state = nullptr;
		wl_surface *_keyboard_focus_surface = nullptr;
		bool _keyboard_focus_accepted = false;

		wayland_pointer _pointer;
		wayland_clipboard *_clipboard = nullptr;
		wayland_overlay_surface _overlay;
		bool _capturing = false;
		double _logged_scale = 1.0;
		wayland_pointer::mode _logged_mode = wayland_pointer::mode::passive;

		friend struct input_test_access;
	};
}
