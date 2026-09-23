// Exercises the production Linux input objects linked from the ReShade sources (see CMake), without
// a compositor, an X server or a Vulkan device.
#include "../source/dll_log.hpp"
#include "../source/linux/wayland_input.hpp"
#include "../source/linux/x11_input.hpp"
#include "../source/linux/window_registry.hpp"
#include "../source/linux/key_translation.hpp"
#include "../source/linux/clipboard.hpp"
#include "../source/linux/paths.hpp"
#include "../source/linux/addon_paths.hpp"
#include "../examples/09-depth/generic_depth_detection.hpp"
#include <glad/vulkan.h>
#include <cassert>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <thread>
#include <sys/mman.h>
#include <sys/wait.h>
#include <linux/input-event-codes.h>

using namespace reshade;

#if !VK_KHR_wayland_surface || !VK_KHR_xcb_surface || !VK_KHR_xlib_surface
#error "Linux builds must expose Wayland, XCB and Xlib Vulkan WSI entry points"
#endif

// Simulated X server window tree, as child -> parent.
static std::unordered_map<xcb_window_t, xcb_window_t> x11_test_parents;
extern "C" xcb_query_tree_cookie_t xcb_query_tree(xcb_connection_t *, xcb_window_t window)
{
	return { window };
}
extern "C" xcb_query_tree_reply_t *xcb_query_tree_reply(xcb_connection_t *, xcb_query_tree_cookie_t cookie, xcb_generic_error_t **)
{
	const auto it = x11_test_parents.find(cookie.sequence);
	if (it == x11_test_parents.end())
		return nullptr;
	auto *const reply = static_cast<xcb_query_tree_reply_t *>(std::calloc(1, sizeof(xcb_query_tree_reply_t)));
	assert(reply != nullptr);
	reply->parent = it->second;
	return reply;
}

void reshade::log::message(level, const char *, ...)
{
}

struct reshade::input_test_access
{
	template <typename backend>
	static void set_focus(backend &target, bool keyboard, bool pointer)
	{
		target._keyboard_focused = keyboard;
		target._pointer_focused = pointer;
	}
	static void set_key_translation(x11_input &target, xcb_keycode_t keycode, xcb_keysym_t keysym, uint32_t utf32 = 0)
	{
		target._key_translations[keycode].keysym = keysym;
		target._key_translations[keycode].utf32 = utf32;
	}
	static bool contains_window(x11_input &target, xcb_window_t root, xcb_window_t window)
	{
		target._root = root;
		return target.contains_window(window);
	}
	static void release_pointer(x11_input &target) { target.release_pointer(); }
	static void release_keyboard(x11_input &target) { target.release_keyboard(); }
};

static wl_surface *const test_surface = reinterpret_cast<wl_surface *>(uintptr_t(0x100));

static void load_us_keymap(wayland_input &backend)
{
	xkb_context *const context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	xkb_rule_names names = {};
	names.layout = "us";
	xkb_keymap *const keymap = xkb_keymap_new_from_names(context, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
	assert(keymap != nullptr);
	char *const text = xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
	const size_t size = std::strlen(text) + 1;
	const int fd = memfd_create("keymap", 0);
	assert(fd >= 0 && write(fd, text, size) == static_cast<ssize_t>(size));
	backend.on_keymap(WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, static_cast<uint32_t>(size));
	std::free(text);
	xkb_keymap_unref(keymap);
	xkb_context_unref(context);
}

static void test_clipboard_offer_mime_types()
{
	wayland_clipboard::offer_info text;
	text.add_mime_type("text/plain");
	text.add_mime_type("text/plain;charset=utf-8");
	text.add_mime_type("UTF8_STRING");
	assert(text.has_text && text.mime_type == "text/plain;charset=utf-8");

	wayland_clipboard::offer_info image;
	image.add_mime_type("image/png");
	assert(!image.has_text && image.mime_type.empty());
}

static void test_clipboard_write()
{
	// A closed receiver must not kill the process, even with default SIGPIPE behavior.
	const pid_t child = fork();
	assert(child >= 0);
	if (child == 0)
	{
		signal(SIGPIPE, SIG_DFL);
		sigset_t signals;
		sigemptyset(&signals);
		sigaddset(&signals, SIGPIPE);
		pthread_sigmask(SIG_UNBLOCK, &signals, nullptr);
		int fds[2];
		assert(pipe(fds) == 0);
		close(fds[0]);
		std::thread([fd = fds[1]] { utils::write_clipboard_text(fd, "closed receiver"); }).join();
		_exit(0);
	}
	int status = 0;
	assert(waitpid(child, &status, 0) == child);
	assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);

	int fds[2];
	assert(pipe(fds) == 0);
	const std::string text(256 * 1024, 'x');
	std::thread writer([fd = fds[1], &text] { utils::write_clipboard_text(fd, text); });
	std::string received;
	char buffer[4096];
	for (ssize_t count; (count = read(fds[0], buffer, sizeof(buffer))) > 0;)
		received.append(buffer, count);
	close(fds[0]);
	writer.join();
	assert(received == text);
}

static void test_translation_tables()
{
	assert(virtual_key_from_keysym(XKB_KEY_a) == 'A');
	assert(virtual_key_from_keysym(XKB_KEY_Z) == 'Z');
	assert(virtual_key_from_keysym(XKB_KEY_5) == '5');
	assert(virtual_key_from_keysym(XKB_KEY_F5) == input::key_f5);
	assert(virtual_key_from_keysym(XKB_KEY_Control_L) == input::key_left_ctrl);
	assert(virtual_key_from_keysym(XKB_KEY_Shift_R) == input::key_right_shift);
	assert(virtual_key_from_keysym(XKB_KEY_Return) == input::key_return);
	assert(virtual_key_from_keysym(XKB_KEY_KP_Home) == input::key_home);
	assert(virtual_key_from_keysym(XKB_KEY_VoidSymbol) == 0);

	assert(virtual_key_from_evdev_button(BTN_LEFT) == input::key_button_left);
	assert(virtual_key_from_evdev_button(BTN_MIDDLE) == input::key_button_middle);
	assert(virtual_key_from_evdev_button(BTN_EXTRA) == input::key_button_xbutton2);
	assert(virtual_key_from_evdev_button(BTN_TASK) == 0);
	assert(virtual_key_from_x11_button(1) == input::key_button_left);
	assert(virtual_key_from_x11_button(2) == input::key_button_middle);
	assert(virtual_key_from_x11_button(3) == input::key_button_right);
	assert(virtual_key_from_x11_button(4) == 0);

	assert(x11_input::keysym_to_utf32('a') == 'a');
	assert(x11_input::keysym_to_utf32(0x010020ACu) == 0x20AC);
	assert(x11_input::keysym_to_utf32(XKB_KEY_Home) == 0);
}

static void test_pointer_absolute_mapping()
{
	wayland_pointer pointer;
	pointer.set_extent(1920, 1080);
	pointer.absolute_motion(960.0, 540.0);
	pointer.end_batch();
	assert(pointer.x() == 960 && pointer.y() == 540);

	// Clamped to the swapchain extent.
	pointer.absolute_motion(3000.0, -10.0);
	pointer.end_batch();
	assert(pointer.x() == 1920 && pointer.y() == 0);
}

static void test_pointer_scale_changes()
{
	wayland_pointer pointer;
	pointer.set_extent(3840, 2160);
	pointer.enter(100.0, 200.0);
	pointer.set_scale(1.5);
	assert(pointer.x() == 150 && pointer.y() == 300);

	// A scale change within a batch applies to the deltas received before it.
	pointer.set_software_cursor(true);
	pointer.relative_motion(10.0, 20.0);
	pointer.set_scale(1.35);
	pointer.end_batch();
	assert(pointer.x() == 149 && pointer.y() == 297);

	pointer.set_scale(0.0);
	pointer.set_scale(std::numeric_limits<double>::quiet_NaN());
	assert(pointer.scale() == 1.35);
	pointer.set_scale(1.0);
	assert(pointer.x() == 110 && pointer.y() == 220);

	pointer.set_software_cursor(false);
	pointer.absolute_motion(100.0, 200.0);
	pointer.set_scale(1.5);
	pointer.end_batch();
	assert(pointer.x() == 150 && pointer.y() == 300);
}

static void test_pointer_relative_session()
{
	wayland_pointer pointer;
	pointer.set_extent(200, 100);
	pointer.enter(20.0, 30.0);
	pointer.end_batch();
	pointer.set_software_cursor(true);

	pointer.relative_motion(5.0, 7.0);
	pointer.end_batch();
	assert(pointer.x() == 25 && pointer.y() == 37);

	// A locked host may keep reporting the same absolute anchor, within or outside the batch.
	pointer.absolute_motion(20.0, 30.0);
	pointer.relative_motion(5.0, 7.0);
	pointer.end_batch();
	assert(pointer.x() == 30 && pointer.y() == 44);
	pointer.absolute_motion(20.0, 30.0);
	pointer.end_batch();
	assert(pointer.x() == 30 && pointer.y() == 44);
	pointer.relative_motion(10.0, 10.0);
	pointer.absolute_motion(80.0, 60.0);
	pointer.end_batch();
	assert(pointer.x() == 40 && pointer.y() == 54);

	// Setting the same state every frame must not reset the accumulated sub-pixel position.
	for (int frame = 0; frame < 4; ++frame)
	{
		pointer.set_software_cursor(true);
		pointer.relative_motion(0.25, 0.0);
		pointer.end_batch();
	}
	assert(pointer.x() == 41 && pointer.y() == 54);

	pointer.relative_motion(500.0, -500.0);
	pointer.end_batch();
	assert(pointer.x() == 200 && pointer.y() == 0);

	// Leaving ends the session, and entering starts from the enter position.
	pointer.relative_motion(-20.0, 0.0);
	pointer.leave();
	pointer.end_batch();
	assert(pointer.x() == 200 && pointer.y() == 0);
	pointer.enter(10.0, 20.0);
	pointer.relative_motion(2.0, 3.0);
	pointer.end_batch();
	assert(pointer.x() == 12 && pointer.y() == 23);

	// Without a software cursor, relative motion is ignored.
	pointer.set_software_cursor(false);
	pointer.absolute_motion(40.0, 50.0);
	pointer.relative_motion(10.0, 10.0);
	pointer.end_batch();
	assert(pointer.x() == 40 && pointer.y() == 50);

	// Servers without relative pointer support keep working with absolute motion.
	pointer.set_software_cursor(true);
	pointer.absolute_motion(60.0, 70.0);
	pointer.end_batch();
	assert(pointer.x() == 60 && pointer.y() == 70);
}

static void test_wayland_scroll()
{
	input owner(nullptr);
	wayland_input backend(owner, nullptr, test_surface);
	backend.on_pointer_enter(test_surface, 0.0, 0.0);

	// Discrete and continuous values of one frame describe the same wheel event.
	backend.on_pointer_axis_discrete(WL_POINTER_AXIS_VERTICAL_SCROLL, 1);
	backend.on_pointer_axis(WL_POINTER_AXIS_VERTICAL_SCROLL, 10.0);
	assert(owner.mouse_wheel_delta() == 0);
	backend.on_pointer_frame();
	assert(owner.mouse_wheel_delta() == -1);
	owner.next_frame();

	backend.on_pointer_axis_discrete(WL_POINTER_AXIS_VERTICAL_SCROLL, -2);
	backend.on_pointer_axis(WL_POINTER_AXIS_VERTICAL_SCROLL, -20.0);
	backend.on_pointer_frame();
	assert(owner.mouse_wheel_delta() == 2);
	owner.next_frame();

	backend.on_pointer_axis(WL_POINTER_AXIS_VERTICAL_SCROLL, 5.0);
	backend.on_pointer_axis(WL_POINTER_AXIS_HORIZONTAL_SCROLL, 5.0);
	backend.on_pointer_frame();
	assert(owner.mouse_wheel_delta() == -1);
	owner.next_frame();
	backend.on_pointer_frame();
	assert(owner.mouse_wheel_delta() == 0);

	backend.on_pointer_leave();
	backend.on_pointer_axis(WL_POINTER_AXIS_VERTICAL_SCROLL, 5.0);
	backend.on_pointer_frame();
	assert(owner.mouse_wheel_delta() == 0);
}

static void test_wayland_parent_keyboard_focus()
{
	input owner(nullptr);
	wayland_input backend(owner, nullptr, test_surface);
	load_us_keymap(backend);
	xkb_context *const context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	xkb_rule_names names = {};
	names.layout = "us";
	xkb_keymap *const keymap = xkb_keymap_new_from_names(context, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
	const uint32_t home = xkb_keymap_key_by_name(keymap, "HOME") - 8;
	const xkb_mod_index_t control_index = xkb_keymap_mod_get_index(keymap, XKB_MOD_NAME_CTRL);
	assert(control_index < 32);
	xkb_keymap_unref(keymap);
	xkb_context_unref(context);

	// Qt focuses its parent surface, which only counts while the pointer is over ours.
	auto *const parent = reinterpret_cast<wl_surface *>(uintptr_t(0x200));
	backend.on_keyboard_enter(parent);
	assert(!backend.keyboard_focused());
	backend.on_pointer_enter(test_surface, 0.0, 0.0);
	assert(backend.keyboard_focused());

	backend.on_key(1, home, WL_KEYBOARD_KEY_STATE_PRESSED);
	backend.on_key(2, home, WL_KEYBOARD_KEY_STATE_RELEASED);
	assert(owner.is_key_pressed(input::key_home) && !owner.is_key_down(input::key_home));
	owner.next_frame();

	// The modifier state at press time is kept, even if released within the same frame.
	backend.on_modifiers(1u << control_index, 0, 0, 0);
	assert(owner.is_key_down(input::key_ctrl));
	backend.on_key(4, home, WL_KEYBOARD_KEY_STATE_PRESSED);
	backend.on_key(5, home, WL_KEYBOARD_KEY_STATE_RELEASED);
	backend.on_modifiers(0, 0, 0, 0);
	assert(owner.is_key_pressed(input::key_home, true, false, false, true));

	backend.on_keyboard_leave();
	assert(!backend.keyboard_focused());
	backend.on_keyboard_enter(parent);
	assert(backend.keyboard_focused());
	backend.on_pointer_leave();
	assert(!backend.keyboard_focused());

	backend.on_keyboard_enter(test_surface);
	assert(backend.keyboard_focused());
}

static void test_x11_keyboard_focus_selection()
{
	constexpr xcb_window_t surface = 0x100;
	constexpr xcb_window_t child = 0x101;
	constexpr xcb_window_t unrelated = 0x200;
	assert(x11_input::select_keyboard_window(surface, surface, true, false) == surface);
	assert(x11_input::select_keyboard_window(surface, child, true, false) == surface);
	assert(x11_input::select_keyboard_window(surface, unrelated, false, true) == unrelated);
	assert(x11_input::select_keyboard_window(surface, unrelated, false, false) == XCB_WINDOW_NONE);
	assert(x11_input::select_keyboard_window(surface, XCB_WINDOW_NONE, false, true) == XCB_WINDOW_NONE);
	assert(x11_input::select_keyboard_window(surface, XCB_INPUT_FOCUS_POINTER_ROOT, false, true) == XCB_WINDOW_NONE);

	input owner(nullptr);
	x11_input backend(owner, 30, nullptr, input::wsi_kind::xcb);
	x11_test_parents = { { 30, 20 }, { 20, 10 }, { 40, 30 }, { 50, 20 } };
	assert(input_test_access::contains_window(backend, 10, 20)); // Focused toolkit parent
	assert(input_test_access::contains_window(backend, 10, 40)); // Focused child
	assert(!input_test_access::contains_window(backend, 10, 50)); // Sibling is not this renderer
	assert(!input_test_access::contains_window(backend, 10, 10)); // Desktop is never owned
	x11_test_parents.clear();
}

static void test_x11_key_and_button_taps()
{
	input owner(nullptr);
	x11_input backend(owner, 30, nullptr, input::wsi_kind::xcb);
	input_test_access::set_key_translation(backend, 10, XKB_KEY_Home);
	input_test_access::set_key_translation(backend, 11, XKB_KEY_Control_L);
	input_test_access::set_key_translation(backend, 12, XKB_KEY_a, 'a');

	backend.on_raw_key(10, true);
	assert(!owner.is_key_down(input::key_home));
	input_test_access::set_focus(backend, true, false);

	// A tap within one frame is reported as both pressed and released, in order.
	backend.on_raw_key(10, true);
	backend.on_raw_key(10, false);
	assert(!owner.is_key_down(input::key_home));
	assert(owner.is_key_pressed(input::key_home) && owner.is_key_released(input::key_home));
	assert(owner.key_transitions().size() == 2);
	assert(owner.key_transitions()[0].down && !owner.key_transitions()[1].down);
	owner.next_frame();
	assert(owner.key_transitions().empty() && !owner.is_key_pressed(input::key_home));

	backend.on_raw_key(11, true);
	backend.on_raw_key(10, true);
	backend.on_raw_key(10, false);
	backend.on_raw_key(11, false);
	assert(owner.is_key_pressed(input::key_ctrl) && owner.is_key_released(input::key_ctrl));
	assert(owner.is_key_pressed(input::key_home, true, false, false, true));
	assert(!owner.is_key_pressed(input::key_home, false, false, false, true));
	owner.next_frame();

	backend.on_raw_key(12, true);
	assert(owner.text_input() == L"a");
	backend.on_raw_key(12, false);
	owner.next_frame();

	backend.on_raw_button(1, true);
	assert(!owner.is_mouse_button_down(0));
	input_test_access::set_focus(backend, true, true);
	backend.on_raw_button(1, true);
	backend.on_raw_button(1, false);
	backend.on_raw_button(4, true);
	assert(owner.is_mouse_button_pressed(0) && owner.is_mouse_button_released(0) && !owner.is_mouse_button_down(0));
	assert(owner.mouse_wheel_delta() == 1);
	owner.next_frame();

	// Focus loss releases held keys of one kind and drops their pending transitions.
	backend.on_raw_button(3, true);
	backend.on_raw_key(10, true);
	input_test_access::release_pointer(backend);
	assert(!owner.is_mouse_button_down(1) && owner.is_mouse_button_released(1));
	assert(owner.is_key_down(input::key_home));
	assert(owner.key_transitions().size() == 1 && owner.key_transitions()[0].key == input::key_home);
	input_test_access::release_keyboard(backend);
	assert(!owner.is_key_down(input::key_home) && owner.key_transitions().empty());
}

static void test_input_lifetime_follows_native_surface()
{
	const input::window_handle window = reinterpret_cast<void *>(uintptr_t(0x1234));
	auto instance = std::make_shared<input>(window);
	const std::weak_ptr<input> observer = instance;
	input::register_surface(window, input::wsi_kind::xcb, nullptr, 0x100, 640, 480);
	input::register_surface(window, input::wsi_kind::xcb, nullptr, 0x200, 640, 480);
	{
		const std::lock_guard<std::mutex> lock(s_windows_mutex);
		s_windows.at(window).input_instance = instance;
	}
	instance.reset();
	assert(!observer.expired());

	input::unregister_surface(window, 0x100);
	assert(!observer.expired());
	input::unregister_surface(window, 0x200);
	assert(observer.expired());
}

static void test_primary_input_handler_claim_transfers()
{
	input owner(nullptr);
	assert(owner.try_acquire_primary_handler());
	assert(!owner.try_acquire_primary_handler());
	owner.release_primary_handler();
	assert(owner.try_acquire_primary_handler());
	owner.release_primary_handler();
}

static void test_paths()
{
	using namespace reshade::utils;
	setenv("RESHADE_TEST_XDG", "/tmp/reshade-xdg", 1);
	assert(xdg_path("RESHADE_TEST_XDG", ".local/share") == "/tmp/reshade-xdg");
	setenv("RESHADE_TEST_XDG", "relative", 1);
	assert(xdg_path("RESHADE_TEST_XDG", ".local/share") == xdg_path("RESHADE_TEST_UNSET", ".local/share"));
	unsetenv("RESHADE_TEST_XDG");

	char directory[] = "/tmp/reshade-path-test.XXXXXX";
	assert(mkdtemp(directory) != nullptr);
	const std::filesystem::path root = directory;
	const auto user = root / "user", installed = root / "prefix/share/reshade";
	std::filesystem::create_directories(user);
	std::filesystem::create_directories(installed);
	std::ofstream(user / "same.addon64").put('x');
	std::ofstream(installed / "same.addon64").put('x');
	std::ofstream(installed / "installed.addon64").put('x');
	std::ofstream(installed / "ignored.txt").put('x');
	const auto files = find_addon_files(user, installed);
	assert(files.size() == 2);
	assert(std::find(files.begin(), files.end(), user / "same.addon64") != files.end());
	assert(std::find(files.begin(), files.end(), installed / "installed.addon64") != files.end());
	assert(find_addon_files(user, {}).size() == 1);
	assert(find_addon_files(root / "missing", installed).size() == 2);
	std::filesystem::remove_all(root);
}

static void test_depth_detection()
{
	using namespace depth_detection;
	assert(clear_evidence(1.0f) == normal);
	assert(clear_evidence(0.0f) == reversed);
	assert(clear_evidence(0.5f) == unknown);
	for (uint8_t convention : { uint8_t(normal), uint8_t(reversed) })
	{
		observation state;
		for (uint64_t frame = 0; frame < 119; ++frame)
		{
			assert(state.observe(1, frame, convention, convention) == none);
			assert(state.observe(1, frame, convention, convention) == none);
		}
		assert(state.frames == 119);
		assert(state.observe(1, 119, convention, convention) == convention);
		assert(state.finished);
		assert(state.observe(1, 120, convention, convention) == none);
	}
	for (uint8_t clears : { uint8_t(none), uint8_t(normal), uint8_t(reversed), uint8_t(normal | reversed), uint8_t(unknown) })
		for (uint8_t comparisons : { uint8_t(none), uint8_t(normal), uint8_t(reversed), uint8_t(normal | reversed), uint8_t(unknown) })
		{
			if ((clears == normal || clears == reversed) && clears == comparisons)
				continue;
			observation state;
			for (uint64_t frame = 0; frame < 600; ++frame)
				assert(state.observe(1, frame, clears, comparisons) == none);
			assert(state.finished && state.frames == 600);
		}
	observation changing;
	for (uint64_t frame = 0; frame < 600; ++frame)
		assert(changing.observe(1 + frame / 100, frame, normal, normal) == none);
	assert(changing.finished);
	observation interrupted;
	for (uint64_t frame = 0; frame < 119; ++frame)
		assert(interrupted.observe(1, frame, normal, normal) == none);
	assert(interrupted.observe(1, 119, unknown, normal) == none);
	assert(interrupted.observe(1, 120, normal, normal) == none);
	assert(interrupted.consistent_frames == 1);
	observation alternating;
	for (uint64_t frame = 0; frame < 600; ++frame)
	{
		const uint8_t vote = frame % 2 ? normal : reversed;
		assert(alternating.observe(1, frame, vote, vote) == none);
	}
	assert(alternating.finished);
	for (uint64_t frame = 600; frame < 800; ++frame)
		assert(alternating.observe(1, frame, normal, normal) == none);
	assert(alternating.frames == 600);
	observation missing;
	assert(missing.observe(0, 0, normal, normal) == none);
	assert(missing.frames == 0);
	assert(clear_evidence(std::numeric_limits<float>::quiet_NaN()) == unknown);
}

int main()
{
	test_clipboard_offer_mime_types();
	test_clipboard_write();
	test_translation_tables();
	test_pointer_absolute_mapping();
	test_pointer_scale_changes();
	test_pointer_relative_session();
	test_wayland_scroll();
	test_wayland_parent_keyboard_focus();
	test_x11_keyboard_focus_selection();
	test_x11_key_and_button_taps();
	test_input_lifetime_follows_native_surface();
	test_primary_input_handler_claim_transfers();
	test_depth_detection();
	test_paths();
	std::cout << "Linux portability tests passed.\n";
}
