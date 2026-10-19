/*
 * SPDX-License-Identifier: BSD-3-Clause OR MIT
 */

#pragma once

#include <cstdint>

namespace reshade::api::wsi
{
	enum class surface_kind : uint64_t { unknown = 0, win32 = 1, wayland = 2, xcb = 3, xlib = 4 };

	// Read with swapchain->get_private_data(<guid>, &value). Set by the Vulkan layer before 'init_swapchain'.
	// The window itself is swapchain::get_hwnd().

	// value = surface_kind
	// 22e69e95-07bb-4b01-ad5b-5eb160871a85
	inline constexpr uint8_t surface_kind_guid[16] = {
		0x22, 0xe6, 0x9e, 0x95, 0x07, 0xbb, 0x4b, 0x01, 0xad, 0x5b, 0x5e, 0xb1, 0x60, 0x87, 0x1a, 0x85 };
}
