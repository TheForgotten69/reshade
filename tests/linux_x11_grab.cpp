// Run only against an isolated X server: this test grabs its pointer and injects input.
#include "linux/x11_input.hpp"
#include <xcb/xtest.h>
#include <cassert>
#include <iostream>
#include <unistd.h>

void reshade::log::message(level, const char *, ...) {}

int main()
{
	if (std::getenv("RESHADE_TEST_X11_ISOLATED") == nullptr)
	{
		std::cerr << "Set RESHADE_TEST_X11_ISOLATED=1 and DISPLAY to an isolated X server; never run on your desktop.\n";
		return 2;
	}
	auto *host = xcb_connect(nullptr, nullptr);
	assert(host && !xcb_connection_has_error(host));
	const auto *screen = xcb_setup_roots_iterator(xcb_get_setup(host)).data;
	const auto window = xcb_generate_id(host);
	xcb_create_window(host, XCB_COPY_FROM_PARENT, window, screen->root, 0, 0, 800, 600, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, 0, nullptr);
	xcb_map_window(host, window);
	xcb_set_input_focus(host, XCB_INPUT_FOCUS_POINTER_ROOT, window, XCB_CURRENT_TIME);
	xcb_warp_pointer(host, XCB_WINDOW_NONE, window, 0, 0, 0, 0, 200, 200);
	free(xcb_get_input_focus_reply(host, xcb_get_input_focus(host), nullptr));
	{
		reshade::input owner(nullptr);
		reshade::x11_input_context backend;
		backend.owner = &owner;
		backend.window = window;
		backend.width = 800;
		backend.height = 600;
		assert(backend.initialize());
		backend.next_frame();
		assert(backend.keyboard_focused && backend.pointer_focused);
		owner.block_mouse_cursor_warping(true);
		const auto start_x = owner.mouse_position_x();
		auto *grab = xcb_grab_pointer_reply(host, xcb_grab_pointer(host, false, window, 0, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, window, XCB_CURSOR_NONE, XCB_CURRENT_TIME), nullptr);
		assert(grab && grab->status == XCB_GRAB_STATUS_SUCCESS);
		free(grab);
		auto *keyboard_grab = xcb_grab_keyboard_reply(host, xcb_grab_keyboard(host, false, window, XCB_CURRENT_TIME, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC), nullptr);
		assert(keyboard_grab && keyboard_grab->status == XCB_GRAB_STATUS_SUCCESS);
		free(keyboard_grab);
		xcb_keycode_t home = 0;
		for (unsigned int key = 1; key < backend.key_translation.size(); ++key)
			if (backend.key_translation[key].keysym == XKB_KEY_Home)
				home = static_cast<xcb_keycode_t>(key);
		assert(home != 0);
		xcb_test_fake_input(host, XCB_KEY_PRESS, home, XCB_CURRENT_TIME, screen->root, 0, 0, 0);
		xcb_test_fake_input(host, XCB_KEY_RELEASE, home, XCB_CURRENT_TIME, screen->root, 0, 0, 0);
		xcb_test_fake_input(host, XCB_MOTION_NOTIFY, 1, XCB_CURRENT_TIME, screen->root, 10, 5, 0);
		xcb_test_fake_input(host, XCB_BUTTON_PRESS, 1, XCB_CURRENT_TIME, screen->root, 0, 0, 0);
		xcb_test_fake_input(host, XCB_BUTTON_RELEASE, 1, XCB_CURRENT_TIME, screen->root, 0, 0, 0);
		free(xcb_get_input_focus_reply(host, xcb_get_input_focus(host), nullptr));
		for (int i = 0; i < 50 && !(owner.is_mouse_button_released(0) && owner.is_key_released(reshade::input::key_home) && owner.mouse_position_x() > start_x); ++i)
		{
			backend.next_frame();
			usleep(1000);
		}
		const bool received = owner.is_mouse_button_pressed(0) && owner.is_mouse_button_released(0);
		xcb_ungrab_pointer(host, XCB_CURRENT_TIME);
		xcb_ungrab_keyboard(host, XCB_CURRENT_TIME);
		xcb_flush(host);
		std::cout << "Mouse tap during host grab: " << (received ? "PASS" : "FAIL") << std::endl;
		assert(received);
		assert(owner.is_key_pressed(reshade::input::key_home) && owner.is_key_released(reshade::input::key_home));
		assert(owner.mouse_position_x() > start_x);
		std::cout << "Home tap and cursor motion during host grabs: PASS" << std::endl;
	}
	xcb_destroy_window(host, window);
	xcb_disconnect(host);
}
