#include "x11_input.hpp"
#include "dll_log.hpp"
#include "key_translation.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <unistd.h>
#include <xcb/shape.h>
#include <xcb/xfixes.h>
#include <xcb/xinput.h>

namespace
{
	// XCB replies, errors and events are malloc'ed and owned by the caller.
	template <typename T>
	auto owned(T *value) { return std::unique_ptr<T, decltype(&std::free)>(value, &std::free); }

	xcb_atom_t intern_atom(xcb_connection_t *connection, const char *name)
	{
		const auto atom = owned(xcb_intern_atom_reply(connection, xcb_intern_atom(connection, false, static_cast<uint16_t>(std::strlen(name)), name), nullptr));
		return atom != nullptr ? atom->atom : static_cast<xcb_atom_t>(XCB_ATOM_NONE);
	}

	void raw_motion_delta(const xcb_input_raw_motion_event_t &event, double delta[2])
	{
		// Raw events only carry the valuators that changed, in axis order.
		const xcb_input_fp3232_t *const values = xcb_input_raw_button_press_axisvalues_raw(&event);
		const uint32_t *const mask = xcb_input_raw_button_press_valuator_mask(&event);
		unsigned int value_index = 0;
		for (unsigned int axis = 0; axis < event.valuators_len * 32u; ++axis)
		{
			if ((mask[axis / 32] & (1u << (axis % 32))) == 0)
				continue;
			if (axis < 2)
				delta[axis] = values[value_index].integral + values[value_index].frac / 4294967296.0;
			++value_index;
		}
	}
}

reshade::x11_input::x11_input(input &owner, xcb_window_t window, void *wsi_display, input::wsi_kind wsi_kind) :
	input_backend(owner),
	_window(window),
	_wsi_display(wsi_display),
	_wsi_kind(wsi_kind)
{
}

reshade::x11_input::~x11_input()
{
	_wine.release_cursor_clip(false);
	if (_connection != nullptr)
		xcb_disconnect(_connection);
}

bool reshade::x11_input::initialize()
{
	int screen = 0;
	_connection = xcb_connect(nullptr, &screen);
	if (_connection == nullptr || xcb_connection_has_error(_connection) != 0)
		return false;

	xcb_screen_iterator_t screen_iterator = xcb_setup_roots_iterator(xcb_get_setup(_connection));
	for (int i = 0; i < screen && screen_iterator.rem != 0; ++i)
		xcb_screen_next(&screen_iterator);
	if (screen_iterator.rem == 0)
		return false;
	_root = screen_iterator.data->root;
	_wm_pid_atom = intern_atom(_connection, "_NET_WM_PID");

	if (owned(xcb_get_geometry_reply(_connection, xcb_get_geometry(_connection, _window), nullptr)) == nullptr)
	{
		log::message(log::level::warning, "X11 input window %#x is not present on the server selected by DISPLAY (WSI display=%p kind=%s).", _window, _wsi_display, _wsi_kind == input::wsi_kind::xcb ? "xcb" : "xlib");
		return false;
	}

	const xcb_query_extension_reply_t *const xinput_extension = xcb_get_extension_data(_connection, &xcb_input_id);
	if (xinput_extension == nullptr || !xinput_extension->present)
		return false;
	_xinput_opcode = xinput_extension->major_opcode;

	// XInput 2.0 suppresses raw events while another client (the host) holds a grab, 2.1 does not.
	const auto version = owned(xcb_input_xi_query_version_reply(_connection, xcb_input_xi_query_version(_connection, 2, 1), nullptr));
	if (version == nullptr || version->major_version < 2)
		return false;
	log::message(log::level::info, "X11 input: negotiated XInput %u.%u.", version->major_version, version->minor_version);
	if (version->major_version == 2 && version->minor_version == 0)
		log::message(log::level::warning, "XInput 2.1 is unavailable; input cannot be observed during host grabs.");

	// XFixes reports every change of the displayed cursor, which tells whether the host hides it.
	const xcb_query_extension_reply_t *const xfixes_extension = xcb_get_extension_data(_connection, &xcb_xfixes_id);
	if (xfixes_extension != nullptr && xfixes_extension->present)
	{
		const auto xfixes_version = owned(xcb_xfixes_query_version_reply(_connection, xcb_xfixes_query_version(_connection, 4, 0), nullptr));
		if (xfixes_version != nullptr && xfixes_version->major_version >= 2)
		{
			_xfixes_first_event = xfixes_extension->first_event;
			xcb_xfixes_select_cursor_input(_connection, _root, XCB_XFIXES_CURSOR_NOTIFY_MASK_DISPLAY_CURSOR);
			update_host_cursor_visibility();
		}
	}

	struct
	{
		xcb_input_event_mask_t header;
		uint32_t mask;
	} raw_events = {
		{ XCB_INPUT_DEVICE_ALL_MASTER, 1 },
		XCB_INPUT_XI_EVENT_MASK_RAW_KEY_PRESS | XCB_INPUT_XI_EVENT_MASK_RAW_KEY_RELEASE |
		XCB_INPUT_XI_EVENT_MASK_RAW_BUTTON_PRESS | XCB_INPUT_XI_EVENT_MASK_RAW_BUTTON_RELEASE |
		XCB_INPUT_XI_EVENT_MASK_RAW_MOTION
	};
	if (const auto error = owned(xcb_request_check(_connection, xcb_input_xi_select_events_checked(_connection, _root, 1, &raw_events.header))))
	{
		log::message(log::level::warning, "XInput2 raw event subscription failed root=%#x error=%u.", _root, error->error_code);
		return false;
	}
	xcb_flush(_connection);

	const xcb_query_extension_reply_t *const shape_extension = xcb_get_extension_data(_connection, &xcb_shape_id);
	_shape_available = shape_extension != nullptr && shape_extension->present;

	_wine_available = _wine.initialize();
	// X11 does not replay focus events that happened before this connection subscribed.
	refresh_keyboard_focus();

	const bool keyboard_ready = cache_key_translations();
	const auto attributes = owned(xcb_get_window_attributes_reply(_connection, xcb_get_window_attributes(_connection, _window), nullptr));
	const auto origin = owned(xcb_translate_coordinates_reply(_connection, xcb_translate_coordinates(_connection, _window, _root, 0, 0), nullptr));
	log::message(log::level::info, "X11 input: xcb_window=%#x viewable=%d origin=(%d, %d) keyboard=%s pointer=%s cursor_tracking=%s capture_shape=%s.",
		_window, attributes != nullptr && attributes->map_state == XCB_MAP_STATE_VIEWABLE, origin != nullptr ? origin->dst_x : 0, origin != nullptr ? origin->dst_y : 0,
		keyboard_ready ? "yes" : "no", _wine_available ? "wine-win32u" : "xinput2", _xfixes_first_event != 0 ? "xfixes" : "no", _shape_available ? "yes" : "no");
	return keyboard_ready;
}

void reshade::x11_input::next_frame()
{
	// XI2 raw events are global, so establish ownership once before draining this frame's events.
	refresh_keyboard_focus();
	update_capture();
	update_keyboard_grab();
	query_pointer();
	// Wine does not see clicks on the capture layer, the raw events deliver them instead.
	if (_wine_available && !_capture_mapped)
		query_wine_buttons();
	_wine.release_cursor_clip(overlay_active());

	bool cursor_changed = false;
	while (xcb_generic_event_t *const event = xcb_poll_for_event(_connection))
	{
		const auto owned_event = owned(event);
		const uint8_t type = event->response_type & 0x7F;
		if (_xfixes_first_event != 0 && type == _xfixes_first_event + XCB_XFIXES_CURSOR_NOTIFY)
			cursor_changed = true;
		// Everything else this connection subscribes to are XI2 raw events.
		if (type != XCB_GE_GENERIC)
			continue;
		const auto *const generic = reinterpret_cast<const xcb_ge_generic_event_t *>(event);
		if (generic->extension != _xinput_opcode)
			continue;

		const auto *const raw = reinterpret_cast<const xcb_input_raw_key_press_event_t *>(event);
		switch (generic->event_type)
		{
		case XCB_INPUT_RAW_KEY_PRESS:
		case XCB_INPUT_RAW_KEY_RELEASE:
			on_raw_key(static_cast<xcb_keycode_t>(raw->detail), generic->event_type == XCB_INPUT_RAW_KEY_PRESS);
			break;
		case XCB_INPUT_RAW_BUTTON_PRESS:
		case XCB_INPUT_RAW_BUTTON_RELEASE:
			on_raw_button(raw->detail, generic->event_type == XCB_INPUT_RAW_BUTTON_PRESS);
			break;
		case XCB_INPUT_RAW_MOTION:
		{
			double delta[2] = {};
			raw_motion_delta(*reinterpret_cast<const xcb_input_raw_motion_event_t *>(event), delta);
			on_raw_motion(delta[0], delta[1]);
			break;
		}
		}
	}
	if (cursor_changed)
		update_host_cursor_visibility();
}

uint32_t reshade::x11_input::keysym_to_utf32(xcb_keysym_t keysym)
{
	// Latin-1 keysyms equal their code point, Unicode keysyms carry it with a 0x01000000 prefix.
	if ((keysym >= 0x20 && keysym <= 0x7E) || (keysym >= 0xA0 && keysym <= 0xFF))
		return keysym;
	if ((keysym & 0xFF000000u) == 0x01000000u)
		return keysym & 0x00FFFFFFu;
	return 0;
}

bool reshade::x11_input::is_cursor_image_visible(const uint32_t *argb_pixels, size_t count)
{
	return std::any_of(argb_pixels, argb_pixels + count, [](uint32_t argb) { return (argb >> 24) != 0; });
}

xcb_window_t reshade::x11_input::select_keyboard_window(xcb_window_t surface_window, xcb_window_t focused_window, bool surface_related, bool wine_related)
{
	if (focused_window == XCB_WINDOW_NONE || focused_window == XCB_INPUT_FOCUS_POINTER_ROOT)
		return XCB_WINDOW_NONE;
	if (surface_related)
		return surface_window;
	return wine_related ? focused_window : static_cast<xcb_window_t>(XCB_WINDOW_NONE);
}

void reshade::x11_input::on_raw_key(xcb_keycode_t keycode, bool pressed)
{
	if (!_keyboard_focused)
		return;

	const key_translation &translation = _key_translations[keycode];
	if (const unsigned int virtual_key = virtual_key_from_keysym(translation.keysym))
		set_key(virtual_key, pressed);

	if (pressed && !is_key_down(input::key_ctrl) && !is_key_down(input::key_alt))
		add_text(is_key_down(input::key_shift) && translation.shifted_utf32 != 0 ? translation.shifted_utf32 : translation.utf32);
}

void reshade::x11_input::on_raw_button(uint32_t button, bool pressed)
{
	if (!_pointer_focused || !_keyboard_focused)
		return;

	// Buttons 4 and 5 are the vertical wheel.
	if (button == 4 || button == 5)
	{
		if (pressed)
			add_wheel_delta(button == 4 ? 1 : -1);
	}
	else if (const unsigned int key = virtual_key_from_x11_button(button))
	{
		set_key(key, pressed);
	}
}

void reshade::x11_input::on_raw_motion(double dx, double dy)
{
	if (!_keyboard_focused || !_pointer_focused || !uses_relative_motion())
		return;

	const auto offset = [](unsigned int position, double delta) { return static_cast<unsigned int>(std::max(0L, std::lround(position + delta))); };
	set_mouse_position(offset(_owner.mouse_position_x(), dx), offset(_owner.mouse_position_y(), dy));
}

bool reshade::x11_input::cache_key_translations()
{
	const xcb_setup_t *const setup = xcb_get_setup(_connection);
	const uint8_t count = static_cast<uint8_t>(setup->max_keycode - setup->min_keycode + 1);
	const auto mapping = owned(xcb_get_keyboard_mapping_reply(_connection, xcb_get_keyboard_mapping(_connection, setup->min_keycode, count), nullptr));
	if (mapping == nullptr || mapping->keysyms_per_keycode == 0)
		return false;

	const xcb_keysym_t *const keysyms = xcb_get_keyboard_mapping_keysyms(mapping.get());
	for (unsigned int index = 0; index < count; ++index)
	{
		const xcb_keysym_t base = keysyms[index * mapping->keysyms_per_keycode];
		const xcb_keysym_t shifted = mapping->keysyms_per_keycode > 1 ? keysyms[index * mapping->keysyms_per_keycode + 1] : base;
		_key_translations[setup->min_keycode + index] = { base != XCB_NO_SYMBOL ? base : shifted, keysym_to_utf32(base), keysym_to_utf32(shifted) };
	}
	return true;
}

bool reshade::x11_input::is_ancestor_window(xcb_window_t ancestor, xcb_window_t window) const
{
	while (window != XCB_WINDOW_NONE && window != XCB_INPUT_FOCUS_POINTER_ROOT)
	{
		if (window == ancestor)
			return true;

		const auto tree = owned(xcb_query_tree_reply(_connection, xcb_query_tree(_connection, window), nullptr));
		if (tree == nullptr || tree->parent == window)
			break;
		window = tree->parent;
	}
	return false;
}

bool reshade::x11_input::contains_window(xcb_window_t window) const
{
	if (window == XCB_WINDOW_NONE || window == XCB_INPUT_FOCUS_POINTER_ROOT || window == _root)
		return false;
	// Toolkits may focus a parent of the Vulkan window (Qt) or one of its children (Wine).
	return is_ancestor_window(_window, window) || is_ancestor_window(window, _window);
}

void reshade::x11_input::refresh_keyboard_focus()
{
	// The host may treat the grab as losing focus, while the keyboard is ours for as long as it lasts.
	if (_keyboard_grabbed)
		return;

	const auto focus = owned(xcb_get_input_focus_reply(_connection, xcb_get_input_focus(_connection), nullptr));
	if (focus == nullptr)
		return;

	// Walking the window tree is expensive, so only re-evaluate ancestry when focus moved.
	const bool surface_related = focus->focus != _last_observed_focus ? contains_window(focus->focus) : _keyboard_window == _window;
	_last_observed_focus = focus->focus;
	const bool wine_related = _wine_available && _wine.is_foreground_process();
	_keyboard_window = select_keyboard_window(_window, focus->focus, surface_related, wine_related);

	const bool focused = _keyboard_window != XCB_WINDOW_NONE;
	if (_keyboard_focused && !focused)
		release_keyboard();
	_keyboard_focused = focused;
}

bool reshade::x11_input::uses_relative_motion() const
{
	return overlay_active() && !_host_cursor_visible && !is_pointer_in_capture();
}

void reshade::x11_input::query_pointer()
{
	wine_input_bridge::point position = {};
	bool focused = false;
	// The host does not see pointer motion over the capture layer (Wine's cursor position goes stale),
	// so query the layer itself, which covers the host's visible window.
	if (_capture_mapped || !_wine.query_pointer_position(position, width(), height(), focused))
	{
		const xcb_window_t window = _capture_mapped ? _capture_window : _window;
		const auto pointer = owned(xcb_query_pointer_reply(_connection, xcb_query_pointer(_connection, window), nullptr));
		if (pointer == nullptr)
			return;
		const unsigned int extent[2] = { _capture_mapped ? _capture_size[0] : width(), _capture_mapped ? _capture_size[1] : height() };
		focused = pointer->same_screen && pointer->win_x >= 0 && pointer->win_y >= 0 && pointer->win_x < static_cast<int>(extent[0]) && pointer->win_y < static_cast<int>(extent[1]);
		// The visible window may be scaled against the swapchain (Wine can render off-screen at another size).
		position = {
			static_cast<int32_t>(static_cast<int64_t>(pointer->win_x) * width() / std::max(1u, extent[0])),
			static_cast<int32_t>(static_cast<int64_t>(pointer->win_y) * height() / std::max(1u, extent[1])) };
	}

	if (_pointer_focused && !focused)
		release_pointer();
	_pointer_focused = focused;
	if (focused && !uses_relative_motion())
		set_mouse_position(static_cast<unsigned int>(position.x), static_cast<unsigned int>(position.y));
}

void reshade::x11_input::query_wine_buttons()
{
	if (!_wine.available() || !_keyboard_focused)
		return;

	// Wine's virtual-key codes for mouse buttons are the ones ReShade uses.
	for (const unsigned int key : mouse_keys)
		set_key(key, _wine.button_down(static_cast<int>(key)));
}

void reshade::x11_input::update_host_cursor_visibility()
{
	// Over the capture layer the displayed cursor is the layer's own invisible one.
	if (_capture_mapped && is_pointer_in_capture())
		return;

	const auto image = owned(xcb_xfixes_get_cursor_image_reply(_connection, xcb_xfixes_get_cursor_image(_connection), nullptr));
	if (image == nullptr)
		return;

	const bool visible = is_cursor_image_visible(xcb_xfixes_get_cursor_image_cursor_image(image.get()), xcb_xfixes_get_cursor_image_cursor_image_length(image.get()));
	if (visible != _host_cursor_visible)
		log::message(log::level::info, "X11 window %#x host cursor %s.", _window, visible ? "shown, following it" : "hidden, drawing the overlay cursor");
	_host_cursor_visible = visible;
}

void reshade::x11_input::update_capture()
{
	const std::vector<input::capture_rect> &regions = pointer_capture();
	if (regions.empty())
	{
		if (_capture_mapped)
		{
			xcb_unmap_window(_connection, _capture_window);
			xcb_flush(_connection);
			_capture_mapped = false;
		}
		return;
	}

	if (!_capture_mapped)
	{
		const xcb_window_t parent = find_visible_window();
		if (parent != _capture_parent)
			log::message(log::level::info, "X11 input capture target %#x for window %#x.", parent, _window);
		if (parent == XCB_WINDOW_NONE || (_capture_window == XCB_WINDOW_NONE && !create_capture_window(parent)))
		{
			_capture_parent = parent;
			return;
		}
		if (parent != _capture_parent)
			xcb_reparent_window(_connection, _capture_window, parent, 0, 0);
		_capture_parent = parent;
	}

	// Follow the host window, whose size only changes together with the swapchain.
	if (!_capture_mapped || _capture_size[0] != width() || _capture_size[1] != height())
	{
		if (const auto geometry = owned(xcb_get_geometry_reply(_connection, xcb_get_geometry(_connection, _capture_parent), nullptr)))
		{
			const uint32_t size[] = { geometry->width, geometry->height };
			xcb_configure_window(_connection, _capture_window, XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, size);
			_capture_size[0] = geometry->width;
			_capture_size[1] = geometry->height;
		}
		_capture_shape.clear();
	}
	apply_capture_shape();

	if (!_capture_mapped)
	{
		const uint32_t stack_mode = XCB_STACK_MODE_ABOVE;
		xcb_configure_window(_connection, _capture_window, XCB_CONFIG_WINDOW_STACK_MODE, &stack_mode);
		xcb_map_window(_connection, _capture_window);
		_capture_mapped = true;
	}
	xcb_flush(_connection);
}

xcb_window_t reshade::x11_input::find_visible_window() const
{
	const auto is_on_screen = [this](xcb_window_t window, uint32_t *area) {
		const auto attributes = owned(xcb_get_window_attributes_reply(_connection, xcb_get_window_attributes(_connection, window), nullptr));
		const auto geometry = owned(xcb_get_geometry_reply(_connection, xcb_get_geometry(_connection, window), nullptr));
		const auto origin = owned(xcb_translate_coordinates_reply(_connection, xcb_translate_coordinates(_connection, window, _root, 0, 0), nullptr));
		if (attributes == nullptr || geometry == nullptr || origin == nullptr || attributes->map_state != XCB_MAP_STATE_VIEWABLE ||
			origin->dst_x + geometry->width <= 0 || origin->dst_y + geometry->height <= 0)
			return false;
		*area = static_cast<uint32_t>(geometry->width) * geometry->height;
		return true;
	};

	// Wine renders Vulkan to an off-screen window and shows the image in a top-level window of its own.
	uint32_t area = 0;
	if (!_wine_available)
		return is_on_screen(_window, &area) ? _window : static_cast<xcb_window_t>(XCB_WINDOW_NONE);

	// winex11 marks its top-level windows with the process ID. Window managers may reparent them.
	const auto is_own_window = [this](xcb_window_t window) {
		const auto property = owned(xcb_get_property_reply(_connection, xcb_get_property(_connection, false, window, _wm_pid_atom, XCB_ATOM_CARDINAL, 0, 1), nullptr));
		return property != nullptr && xcb_get_property_value_length(property.get()) == 4 &&
			*static_cast<const uint32_t *>(xcb_get_property_value(property.get())) == static_cast<uint32_t>(getpid());
	};
	xcb_window_t best = XCB_WINDOW_NONE;
	uint32_t best_area = 0;
	const auto tree = owned(xcb_query_tree_reply(_connection, xcb_query_tree(_connection, _root), nullptr));
	for (int i = 0; tree != nullptr && i < xcb_query_tree_children_length(tree.get()); ++i)
	{
		xcb_window_t candidate = xcb_query_tree_children(tree.get())[i];
		if (!is_own_window(candidate))
		{
			const auto children = owned(xcb_query_tree_reply(_connection, xcb_query_tree(_connection, candidate), nullptr));
			if (children == nullptr || xcb_query_tree_children_length(children.get()) != 1 || !is_own_window(xcb_query_tree_children(children.get())[0]))
				continue;
			candidate = xcb_query_tree_children(children.get())[0];
		}
		if (candidate != _window && is_on_screen(candidate, &area) && area > best_area)
		{
			best = candidate;
			best_area = area;
		}
	}
	return best;
}

bool reshade::x11_input::create_capture_window(xcb_window_t parent)
{
	// A cursor made of an empty 1x1 bitmap hides the host's cursor while it is over the layer.
	const xcb_pixmap_t blank = xcb_generate_id(_connection);
	xcb_create_pixmap(_connection, 1, blank, _window, 1, 1);
	const xcb_cursor_t invisible_cursor = xcb_generate_id(_connection);
	xcb_create_cursor(_connection, invisible_cursor, blank, blank, 0, 0, 0, 0, 0, 0, 0, 0);
	xcb_free_pixmap(_connection, blank);

	// Pointer events go to the deepest window selecting them and do not propagate further, so
	// selecting them here is what takes them away from the host.
	const uint32_t values[] = {
		XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_LEAVE_WINDOW,
		invisible_cursor
	};
	_capture_window = xcb_generate_id(_connection);
	const xcb_void_cookie_t cookie = xcb_create_window_checked(_connection, 0, _capture_window, parent, 0, 0, width(), height(), 0,
		XCB_WINDOW_CLASS_INPUT_ONLY, XCB_COPY_FROM_PARENT, XCB_CW_EVENT_MASK | XCB_CW_CURSOR, values);
	xcb_free_cursor(_connection, invisible_cursor);
	if (const auto error = owned(xcb_request_check(_connection, cookie)))
	{
		log::message(log::level::warning, "Failed to create X11 input capture window on %#x with error %u.", parent, error->error_code);
		_capture_window = XCB_WINDOW_NONE;
		return false;
	}
	_capture_parent = parent;
	log::message(log::level::info, "X11 input capture window %#x covers %#x.", _capture_window, parent);
	return true;
}

void reshade::x11_input::apply_capture_shape()
{
	const std::vector<input::capture_rect> &regions = pointer_capture();
	const auto equal = [](const input::capture_rect &lhs, const input::capture_rect &rhs) {
		return lhs.x == rhs.x && lhs.y == rhs.y && lhs.width == rhs.width && lhs.height == rhs.height;
	};
	if (!_shape_available || std::equal(regions.begin(), regions.end(), _capture_shape.begin(), _capture_shape.end(), equal))
		return;

	std::vector<xcb_rectangle_t> rectangles;
	rectangles.reserve(regions.size());
	for (const input::capture_rect &region : regions)
		rectangles.push_back({
			static_cast<int16_t>(std::lround(region.x * _capture_size[0])), static_cast<int16_t>(std::lround(region.y * _capture_size[1])),
			static_cast<uint16_t>(std::lround(region.width * _capture_size[0])), static_cast<uint16_t>(std::lround(region.height * _capture_size[1])) });
	xcb_shape_rectangles(_connection, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_INPUT, XCB_CLIP_ORDERING_UNSORTED, _capture_window, 0, 0, static_cast<uint32_t>(rectangles.size()), rectangles.data());
	_capture_shape = regions;
}

void reshade::x11_input::update_keyboard_grab()
{
	const bool wanted = _owner.is_blocking_keyboard_input() && _keyboard_focused;
	if (wanted == _keyboard_grabbed)
		return;

	if (!wanted)
	{
		xcb_ungrab_keyboard(_connection, XCB_CURRENT_TIME);
		xcb_flush(_connection);
		_keyboard_grabbed = false;
		return;
	}

	// A key held while the grab starts would never be released for the host (Windows ReShade does not
	// block a key up either if the key down reached the application), and a failed grab is retried.
	if (_owner.is_any_key_down() || (_keyboard_grab_retry_delay != 0 && --_keyboard_grab_retry_delay != 0))
		return;

	const auto grab = owned(xcb_grab_keyboard_reply(_connection, xcb_grab_keyboard(_connection, false, _window, XCB_CURRENT_TIME, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC), nullptr));
	_keyboard_grabbed = grab != nullptr && grab->status == XCB_GRAB_STATUS_SUCCESS;
	if (!_keyboard_grabbed)
		_keyboard_grab_retry_delay = 30;
}
