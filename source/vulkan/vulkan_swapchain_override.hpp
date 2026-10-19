/*
 * Copyright (C) 2014 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause OR MIT
 */

#pragma once

#include <algorithm>
#include <glad/vulkan.h>
#include "reshade_api_format.hpp"

namespace reshade::vulkan
{
	// Whether the surface lists this exact (format, color space) pair.
	inline bool surface_format_supported(const VkSurfaceFormatKHR *formats, uint32_t count, VkFormat format, VkColorSpaceKHR color_space)
	{
		return std::any_of(formats, formats + count,
			[=](const VkSurfaceFormatKHR &f) { return f.format == format && f.colorSpace == color_space; });
	}

	// Whether a VkImageFormatListCreateInfo list still contains the format; when it does not, the list and the mutable-format flag must be dropped together.
	inline bool view_format_list_contains(const VkFormat *formats, uint32_t count, VkFormat format)
	{
		return std::find(formats, formats + count, format) != formats + count;
	}

	// The proxy sRGB blit reads the application's image through an sRGB view when one exists, which only works for formats with distinct sRGB and linear variants.
	inline bool proxy_blit_decodes_srgb(reshade::api::format app_format)
	{
		return reshade::api::format_to_default_typed(app_format, 1) != reshade::api::format_to_default_typed(app_format, 0);
	}
}
