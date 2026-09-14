// Exercise the real Wayland/X11 backend logic linked from the production translation units below
// (see CMakeLists), without a compositor or a Vulkan device. Deliberately does not '#include' any
// production '.cpp' - only the headers that declare the types and free functions under test - so
// this stays a real link-time test of the shipped object code rather than a second copy of it.
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
#include <fstream>
#include <iostream>
#include <limits>
#include <sys/wait.h>

using namespace reshade;

#if !VK_KHR_wayland_surface || !VK_KHR_xcb_surface || !VK_KHR_xlib_surface
#error "Linux builds must expose Wayland, XCB and Xlib Vulkan WSI entry points"
#endif

static uint32_t pointer_version = 5;
extern "C" uint32_t wl_proxy_get_version(wl_proxy *)
{
	return pointer_version;
}

void reshade::log::message(level, const char *, ...)
{
}

static void test_clipboard()
{
	reshade::wayland_input_context context;
	auto *const text_offer = reinterpret_cast<wl_data_offer *>(static_cast<uintptr_t>(0x100));
	auto *const image_offer = reinterpret_cast<wl_data_offer *>(static_cast<uintptr_t>(0x200));
	context.data_offers.try_emplace(text_offer);
	context.data_offers.try_emplace(image_offer);
	reshade::wayland_input_context::data_offer_offer(&context, text_offer, "text/plain;charset=utf-8");
	reshade::wayland_input_context::data_offer_offer(&context, image_offer, "image/png");
	assert(context.data_offers.at(text_offer).has_text);
	assert(context.data_offers.at(text_offer).mime_type == "text/plain;charset=utf-8");
	assert(!context.data_offers.at(image_offer).has_text);
	context.data_offers.clear();

	context.clipboard_text = "ReShade clipboard";
	context.clipboard_source = reinterpret_cast<wl_data_source *>(&context);
	assert(context.get_clipboard_text() == context.clipboard_text);
	context.clipboard_source = nullptr;

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
		std::thread([fd = fds[1]] { reshade::utils::write_clipboard_text(fd, "closed receiver"); }).join();
		_exit(0);
	}
	int status = 0;
	assert(waitpid(child, &status, 0) == child);
	assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);

	int fds[2];
	assert(pipe(fds) == 0);
	const std::string text(256 * 1024, 'x');
	std::thread writer([fd = fds[1], &text] { reshade::utils::write_clipboard_text(fd, text); });
	std::string received;
	char buffer[4096];
	for (ssize_t count; (count = read(fds[0], buffer, sizeof(buffer))) > 0;)
		received.append(buffer, count);
	close(fds[0]);
	writer.join();
	assert(received == text);
}

static void test_scroll()
{
	reshade::input input(nullptr);
	reshade::wayland_input_context context;
	context.owner = &input;
	context.pointer_focused = true;
	using callbacks = reshade::wayland_input_context;

	callbacks::pointer_axis_discrete(&context, nullptr, WL_POINTER_AXIS_VERTICAL_SCROLL, 1);
	callbacks::pointer_axis(&context, nullptr, 0, WL_POINTER_AXIS_VERTICAL_SCROLL, wl_fixed_from_int(10));
	assert(input.mouse_wheel_delta() == 0);
	callbacks::pointer_frame(&context, nullptr);
	assert(input.mouse_wheel_delta() == -1);
	input.next_frame();

	callbacks::pointer_axis_discrete(&context, nullptr, WL_POINTER_AXIS_VERTICAL_SCROLL, -2);
	callbacks::pointer_axis(&context, nullptr, 0, WL_POINTER_AXIS_VERTICAL_SCROLL, wl_fixed_from_int(-20));
	callbacks::pointer_frame(&context, nullptr);
	assert(input.mouse_wheel_delta() == 2);
	input.next_frame();

	callbacks::pointer_axis(&context, nullptr, 0, WL_POINTER_AXIS_VERTICAL_SCROLL, wl_fixed_from_int(5));
	callbacks::pointer_frame(&context, nullptr);
	assert(input.mouse_wheel_delta() == -1);
	input.next_frame();
	callbacks::pointer_frame(&context, nullptr);
	assert(input.mouse_wheel_delta() == 0);

	pointer_version = 4;
	callbacks::pointer_axis(&context, nullptr, 0, WL_POINTER_AXIS_VERTICAL_SCROLL, wl_fixed_from_int(10));
	assert(input.mouse_wheel_delta() == -1);
}

static void test_key_translation()
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
	assert(x11_input_context::keysym_to_utf32('a') == 'a');
	assert(x11_input_context::keysym_to_utf32(0x010020acu) == 0x20ac);
	assert(x11_input_context::keysym_to_utf32(XKB_KEY_Home) == 0);
}

// Regression test for the fractional-scale pointer desync (surface-local logical pointer
// coordinates were passed straight through as if they were already physical framebuffer pixels):
// 'to_framebuffer_pointer_position' must scale by the known output ratio, clamp to the
// framebuffer extent, and fall back to passing coordinates through unscaled when no reliable
// ratio is known rather than guessing one.
static void test_pointer_coordinate_scaling()
{
	wayland_input_context context;

	// Verified against a live KWin session: a client that never opts into a Wayland buffer scale
	// (true of most Vulkan applications, e.g. vkcube) has a 1:1 logical/pixel surface, so
	// coordinates must pass through unscaled regardless of 'output_scale' - multiplying by it
	// previously clamped the cursor short of the real window edge on exactly this common case.
	context.output_scale = 0.0;
	assert(context.to_framebuffer_pointer_position(960.0, 1920) == 960.0);
	context.output_scale = 1.5;
	assert(context.to_framebuffer_pointer_position(960.0, 2880) == 960.0);

	// Still clamped to the framebuffer extent.
	assert(context.to_framebuffer_pointer_position(3000.0, 1920) == 1920.0);
	assert(context.to_framebuffer_pointer_position(-10.0, 1920) == 0.0);
}

static void test_input_lifetime_follows_native_surface()
{
	const reshade::input::window_handle window = reinterpret_cast<void *>(static_cast<uintptr_t>(0x1234));
	auto instance = std::make_shared<reshade::input>(window);
	const std::weak_ptr<reshade::input> observer = instance;

	{
		std::lock_guard<std::mutex> lock(s_x11_windows_mutex);
		s_x11_windows[window].input_instance = instance;
	}
	instance.reset();
	assert(!observer.expired());

	reshade::input::unregister_x11_window(window);
	assert(observer.expired());
}

static void test_primary_input_handler_claim_transfers()
{
	reshade::input input(nullptr);
	assert(input.try_acquire_primary_handler());
	assert(!input.try_acquire_primary_handler());
	input.release_primary_handler();
	assert(input.try_acquire_primary_handler());
	input.release_primary_handler();
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
	test_depth_detection();
	test_clipboard();
	test_scroll();
	test_key_translation();
	test_pointer_coordinate_scaling();
	test_input_lifetime_follows_native_surface();
	test_primary_input_handler_claim_transfers();
	test_paths();
	std::cout << "Depth detection, clipboard, scroll, key translation, pointer scaling and add-on path tests passed.\n";
}
