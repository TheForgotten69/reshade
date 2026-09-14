/*
 * Copyright (C) 2014 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause OR MIT
 */

#include "vulkan_hooks.hpp"
#include "vulkan_impl_device.hpp"
#include "vulkan_impl_command_queue.hpp"
#include "vulkan_impl_swapchain.hpp"
#include "vulkan_impl_type_convert.hpp"
#include "dll_log.hpp"
#include "addon_manager.hpp"
#include "runtime_manager.hpp"
#include "input.hpp"
#include "lockfree_linear_map.hpp"
#include <algorithm> // std::fill_n, std::sort, std::unique
#include <atomic>

#define vk device_impl->_dispatch_table

extern thread_local bool g_in_dxgi_runtime;

extern lockfree_linear_map<VkSurfaceKHR, vulkan_surface, 16> g_vulkan_surfaces;
extern lockfree_linear_map<void *, reshade::vulkan::device_impl *, 8> g_vulkan_devices;

#if RESHADE_ADDON
extern void create_default_view(reshade::vulkan::device_impl *device_impl, VkImage image);
extern void destroy_default_view(reshade::vulkan::device_impl *device_impl, VkImage image);
#endif

static void destroy_proxy_images(reshade::vulkan::device_impl *device_impl, reshade::vulkan::object_data<VK_OBJECT_TYPE_SWAPCHAIN_KHR> *swapchain_impl)
{
	for (const reshade::api::resource image : swapchain_impl->_proxy_images)
		device_impl->destroy_resource(image);
	for (const reshade::api::resource image : swapchain_impl->_proxy_srgb_images)
		device_impl->destroy_resource(image);
	swapchain_impl->_proxy_images.clear();
	swapchain_impl->_proxy_srgb_images.clear();
	swapchain_impl->_proxy_srgb_images_initialized.clear();
}

static void retire_swapchain(reshade::vulkan::object_data<VK_OBJECT_TYPE_SWAPCHAIN_KHR> *swapchain_impl)
{
	if (swapchain_impl == nullptr || swapchain_impl->_retired)
		return;

	// Vulkan permits presenting an image acquired from the old swapchain after a replacement
	// is created. Keep the object and image metadata alive until vkDestroySwapchainKHR, but
	// release the effect runtime so ReShade never renders into a retired swapchain.
	reshade::reset_effect_runtime(swapchain_impl);
#if RESHADE_ADDON
	reshade::invoke_addon_event<reshade::addon_event::destroy_swapchain>(swapchain_impl, false);
#endif
	reshade::destroy_effect_runtime(swapchain_impl);
	swapchain_impl->_runtime_destroyed = true;
	swapchain_impl->_retired = true;
}

#if VK_KHR_swapchain
VkResult VKAPI_CALL vkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkSwapchainKHR *pSwapchain)
{
	reshade::log::message(reshade::log::level::info, "Redirecting vkCreateSwapchainKHR(device = %p, pCreateInfo = %p, pAllocator = %p, pSwapchain = %p) ...", device, pCreateInfo, pAllocator, pSwapchain);

	reshade::vulkan::device_impl *const device_impl = g_vulkan_devices.at(dispatch_key_from_handle(device));
	RESHADE_VULKAN_GET_DEVICE_DISPATCH_PTR(CreateSwapchainKHR, device_impl);

	assert(pCreateInfo != nullptr && pSwapchain != nullptr);

	VkSwapchainCreateInfoKHR create_info = *pCreateInfo;

	std::vector<VkFormat> format_list;
	VkImageFormatListCreateInfo format_list_info;
	std::vector<uint32_t> queue_family_list;

	// Only have to enable additional features if there is a graphics queue, since ReShade will not run otherwise
	if (device_impl->_primary_graphics_queue != nullptr)
	{
		// Add required usage flags to create info
		create_info.imageUsage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

#if VK_KHR_swapchain_mutable_format
		// Add required format variants, so e.g. both linear and sRGB views can be created for the swap chain images
		format_list.push_back(reshade::vulkan::convert_format(reshade::api::format_to_default_typed(reshade::vulkan::convert_format(create_info.imageFormat), 0)));
		format_list.push_back(reshade::vulkan::convert_format(reshade::api::format_to_default_typed(reshade::vulkan::convert_format(create_info.imageFormat), 1)));

		// Only have to make format mutable if they are actually different
		if (format_list[0] != format_list[1])
			create_info.flags |= VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR;

		// Patch the format list in the create info of the application
		if (const auto existing_format_list_info = find_in_structure_chain<VkImageFormatListCreateInfo>(
				pCreateInfo->pNext, VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO))
		{
			format_list.insert(format_list.end(), existing_format_list_info->pViewFormats, existing_format_list_info->pViewFormats + existing_format_list_info->viewFormatCount);

			// Remove duplicates from the list (since the new formats may have already been added by the application)
			std::sort(format_list.begin(), format_list.end());
			format_list.erase(std::unique(format_list.begin(), format_list.end()), format_list.end());

			const_cast<VkImageFormatListCreateInfo *>(existing_format_list_info)->viewFormatCount = static_cast<uint32_t>(format_list.size());
			const_cast<VkImageFormatListCreateInfo *>(existing_format_list_info)->pViewFormats = format_list.data();
		}
		else if (format_list[0] != format_list[1])
		{
			format_list_info = { VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO };
			format_list_info.pNext = create_info.pNext;
			format_list_info.viewFormatCount = static_cast<uint32_t>(format_list.size());
			format_list_info.pViewFormats = format_list.data();

			create_info.pNext = &format_list_info;
		}
#endif

		// Add required queue family indices, so images can be used on the graphics queue
		if (create_info.imageSharingMode == VK_SHARING_MODE_CONCURRENT)
		{
			queue_family_list.reserve(create_info.queueFamilyIndexCount + 1);
			queue_family_list.push_back(device_impl->_primary_graphics_queue_family_index);

			for (uint32_t i = 0; i < create_info.queueFamilyIndexCount; ++i)
				if (create_info.pQueueFamilyIndices[i] != device_impl->_primary_graphics_queue_family_index)
					queue_family_list.push_back(create_info.pQueueFamilyIndices[i]);

			create_info.queueFamilyIndexCount = static_cast<uint32_t>(queue_family_list.size());
			create_info.pQueueFamilyIndices = queue_family_list.data();
		}
	}

	// Dump swap chain description
	{
		const char *format_string = nullptr;
		switch (create_info.imageFormat)
		{
		case VK_FORMAT_UNDEFINED:
			format_string = "VK_FORMAT_UNDEFINED";
			break;
		case VK_FORMAT_R8G8B8A8_UNORM:
			format_string = "VK_FORMAT_R8G8B8A8_UNORM";
			break;
		case VK_FORMAT_R8G8B8A8_SRGB:
			format_string = "VK_FORMAT_R8G8B8A8_SRGB";
			break;
		case VK_FORMAT_B8G8R8A8_UNORM:
			format_string = "VK_FORMAT_B8G8R8A8_UNORM";
			break;
		case VK_FORMAT_B8G8R8A8_SRGB:
			format_string = "VK_FORMAT_B8G8R8A8_SRGB";
			break;
		case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
			format_string = "VK_FORMAT_A2B10G10R10_UNORM_PACK32";
			break;
		case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
			format_string = "VK_FORMAT_A2R10G10B10_UNORM_PACK32";
			break;
		case VK_FORMAT_R16G16B16A16_UNORM:
			format_string = "VK_FORMAT_R16G16B16A16_UNORM";
			break;
		case VK_FORMAT_R16G16B16A16_SFLOAT:
			format_string = "VK_FORMAT_R16G16B16A16_SFLOAT";
			break;
		}

		const char *color_space_string = nullptr;
		switch (create_info.imageColorSpace)
		{
		case VK_COLOR_SPACE_SRGB_NONLINEAR_KHR:
			color_space_string = "VK_COLOR_SPACE_SRGB_NONLINEAR_KHR";
			break;
#if VK_EXT_swapchain_colorspace
		case VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT:
			color_space_string = "VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT";
			break;
		case VK_COLOR_SPACE_BT2020_LINEAR_EXT:
			color_space_string = "VK_COLOR_SPACE_BT2020_LINEAR_EXT";
			break;
		case VK_COLOR_SPACE_HDR10_ST2084_EXT:
			color_space_string = "VK_COLOR_SPACE_HDR10_ST2084_EXT";
			break;
		case VK_COLOR_SPACE_HDR10_HLG_EXT:
			color_space_string = "VK_COLOR_SPACE_HDR10_HLG_EXT";
			break;
#endif
		}

		reshade::log::message(reshade::log::level::info, "> Dumping swap chain description:");
		reshade::log::message(reshade::log::level::info, "  +-----------------------------------------+-----------------------------------------+");
		reshade::log::message(reshade::log::level::info, "  | Parameter                               | Value                                   |");
		reshade::log::message(reshade::log::level::info, "  +-----------------------------------------+-----------------------------------------+");
		reshade::log::message(reshade::log::level::info, "  | flags                                   |"                               " %-#39x |", static_cast<unsigned int>(create_info.flags));
		reshade::log::message(reshade::log::level::info, "  | surface                                 |"                                " %-39p |", create_info.surface);
		reshade::log::message(reshade::log::level::info, "  | minImageCount                           |"                                " %-39u |", create_info.minImageCount);
		if (format_string != nullptr)
		reshade::log::message(reshade::log::level::info, "  | imageFormat                             |"                                " %-39s |", format_string);
		else
		reshade::log::message(reshade::log::level::info, "  | imageFormat                             |"                                " %-39d |", static_cast<int>(create_info.imageFormat));
		if (color_space_string != nullptr)
		reshade::log::message(reshade::log::level::info, "  | imageColorSpace                         |"                                " %-39s |", color_space_string);
		else
		reshade::log::message(reshade::log::level::info, "  | imageColorSpace                         |"                                " %-39d |", static_cast<int>(create_info.imageColorSpace));
		reshade::log::message(reshade::log::level::info, "  | imageExtent                             |"            " %-19u"            " %-19u |", create_info.imageExtent.width, create_info.imageExtent.height);
		reshade::log::message(reshade::log::level::info, "  | imageArrayLayers                        |"                                " %-39u |", create_info.imageArrayLayers);
		reshade::log::message(reshade::log::level::info, "  | imageUsage                              |"                               " %-#39x |", static_cast<unsigned int>(create_info.imageUsage));
		reshade::log::message(reshade::log::level::info, "  | imageSharingMode                        |"                                " %-39d |", static_cast<int>(create_info.imageSharingMode));
		reshade::log::message(reshade::log::level::info, "  | queueFamilyIndexCount                   |"                                " %-39u |", create_info.queueFamilyIndexCount);
		reshade::log::message(reshade::log::level::info, "  | preTransform                            |"                               " %-#39x |", static_cast<unsigned int>(create_info.preTransform));
		reshade::log::message(reshade::log::level::info, "  | compositeAlpha                          |"                               " %-#39x |", static_cast<unsigned int>(create_info.compositeAlpha));
		reshade::log::message(reshade::log::level::info, "  | presentMode                             |"                                " %-39d |", static_cast<int>(create_info.presentMode));
		reshade::log::message(reshade::log::level::info, "  | clipped                                 |"                                " %-39s |", create_info.clipped ? "true" : "false");
		reshade::log::message(reshade::log::level::info, "  | oldSwapchain                            |"                                " %-39p |", create_info.oldSwapchain);
		reshade::log::message(reshade::log::level::info, "  +-----------------------------------------+-----------------------------------------+");
	}

	// Look up window handle from surface
	const vulkan_surface surface_info = g_vulkan_surfaces.at(create_info.surface);
	void *const hwnd = surface_info.window;
	const VkSwapchainCreateInfoKHR application_create_info = create_info;
	bool use_proxy_images = false;

#if RESHADE_ADDON
	reshade::api::swapchain_desc desc = {};
	desc.back_buffer.type = reshade::api::resource_type::texture_2d;
	desc.back_buffer.texture.width = create_info.imageExtent.width;
	desc.back_buffer.texture.height = create_info.imageExtent.height;
	assert(create_info.imageArrayLayers <= std::numeric_limits<uint16_t>::max());
	desc.back_buffer.texture.depth_or_layers = static_cast<uint16_t>(create_info.imageArrayLayers);
	desc.back_buffer.texture.levels = 1;
	desc.back_buffer.texture.format = reshade::vulkan::convert_format(create_info.imageFormat);
	desc.back_buffer.texture.samples = 1;
	desc.back_buffer.heap = reshade::api::memory_heap::default_;
	reshade::vulkan::convert_image_usage_flags_to_usage(create_info.imageUsage, desc.back_buffer.usage);

	desc.back_buffer_count = create_info.minImageCount;
	desc.present_mode = static_cast<uint32_t>(create_info.presentMode);
	desc.present_flags = create_info.flags;
	desc.sync_interval = create_info.presentMode == VK_PRESENT_MODE_IMMEDIATE_KHR ? 0 : UINT32_MAX;
	desc.color_space = reshade::vulkan::convert_color_space(create_info.imageColorSpace);

#if VK_EXT_full_screen_exclusive
	// Optionally change fullscreen state
	VkSurfaceFullScreenExclusiveInfoEXT fullscreen_info;
	if (const auto existing_fullscreen_info = find_in_structure_chain<VkSurfaceFullScreenExclusiveInfoEXT>(
			pCreateInfo->pNext, VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT))
	{
		fullscreen_info = *existing_fullscreen_info;

		desc.fullscreen_state = existing_fullscreen_info->fullScreenExclusive == VK_FULL_SCREEN_EXCLUSIVE_ALLOWED_EXT;
	}
	else
	{
		fullscreen_info = { VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT, const_cast<void *>(create_info.pNext) };
		fullscreen_info.fullScreenExclusive = VK_FULL_SCREEN_EXCLUSIVE_DEFAULT_EXT;
	}
#endif

	if (reshade::invoke_addon_event<reshade::addon_event::create_swapchain>(reshade::api::device_api::vulkan, desc, hwnd))
	{
		// A Vulkan application bakes the swap chain format into render passes and
		// pipelines. Keep its original images when an add-on changes only the WSI
		// output format, rather than exposing the new format to the application.
		use_proxy_images =
			desc.back_buffer.texture.format != reshade::vulkan::convert_format(application_create_info.imageFormat) ||
			desc.color_space != reshade::vulkan::convert_color_space(application_create_info.imageColorSpace);

		create_info.imageFormat = reshade::vulkan::convert_format(desc.back_buffer.texture.format);
		create_info.imageColorSpace = reshade::vulkan::convert_color_space(desc.color_space);
		create_info.imageExtent.width = desc.back_buffer.texture.width;
		create_info.imageExtent.height = desc.back_buffer.texture.height;
		create_info.imageArrayLayers = desc.back_buffer.texture.depth_or_layers;
		reshade::vulkan::convert_usage_to_image_usage_flags(desc.back_buffer.usage, create_info.imageUsage);

		create_info.minImageCount = desc.back_buffer_count;
		create_info.presentMode = static_cast<VkPresentModeKHR>(desc.present_mode);
		create_info.flags = static_cast<uint32_t>(desc.present_flags);

#if VK_EXT_full_screen_exclusive
		if (desc.fullscreen_state)
		{
			if (fullscreen_info.fullScreenExclusive != VK_FULL_SCREEN_EXCLUSIVE_APPLICATION_CONTROLLED_EXT)
			{
				fullscreen_info.fullScreenExclusive = VK_FULL_SCREEN_EXCLUSIVE_ALLOWED_EXT;

				create_info.pNext = &fullscreen_info;
			}
		}
		else
		{
			if (fullscreen_info.fullScreenExclusive == VK_FULL_SCREEN_EXCLUSIVE_ALLOWED_EXT)
			{
				fullscreen_info.fullScreenExclusive = VK_FULL_SCREEN_EXCLUSIVE_APPLICATION_CONTROLLED_EXT;

				create_info.pNext = &fullscreen_info;
			}
		}
#endif

		if (desc.sync_interval == 0)
			create_info.presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;

		// Remove format list info if format was overridden
		if (const auto existing_format_list_info = find_in_structure_chain<VkImageFormatListCreateInfo>(
				create_info.pNext, VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO))
		{
			if (const VkFormat *const formats_begin = existing_format_list_info->pViewFormats, *const formats_end = existing_format_list_info->pViewFormats + existing_format_list_info->viewFormatCount;
				std::find(formats_begin, formats_end, create_info.imageFormat) == formats_end)
			{
				const_cast<VkImageFormatListCreateInfo *>(existing_format_list_info)->viewFormatCount = 0;
			}
		}
	}
#endif

	if (use_proxy_images)
	{
		// ReShade copies the completed application image into the actual WSI image
		// immediately before it renders effects and presents it.
		create_info.imageUsage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		reshade::log::message(reshade::log::level::info, "Using Vulkan proxy swapchain images to preserve the application's render format.");
	}

	// The old handle remains valid for presentation of previously acquired images. Do not
	// unregister or reuse its object: both old and replacement handles can coexist.
	reshade::vulkan::object_data<VK_OBJECT_TYPE_SWAPCHAIN_KHR> *old_swapchain_impl = nullptr;
	if (create_info.oldSwapchain != VK_NULL_HANDLE)
		old_swapchain_impl = device_impl->get_private_data_for_object<VK_OBJECT_TYPE_SWAPCHAIN_KHR, true>(create_info.oldSwapchain);

	assert(!g_in_dxgi_runtime);
	g_in_dxgi_runtime = true;
	const VkResult result = trampoline(device, &create_info, pAllocator, pSwapchain);
	g_in_dxgi_runtime = false;

	// The Vulkan specification retires 'oldSwapchain' whether creation succeeds or fails.
	// Retire our matching runtime only after the driver has observed the replacement request.
	retire_swapchain(old_swapchain_impl);
	if (result < VK_SUCCESS)
	{
		reshade::log::message(reshade::log::level::warning, "vkCreateSwapchainKHR failed with error code %d.", static_cast<int>(result));
		return result;
	}

#if defined(__linux__)
	if (surface_info.kind == vulkan_wsi_kind::wayland)
		reshade::input::register_wayland_surface(hwnd, surface_info.display, reinterpret_cast<uintptr_t>(create_info.surface), create_info.imageExtent.width, create_info.imageExtent.height);
	else if (surface_info.kind == vulkan_wsi_kind::xcb || surface_info.kind == vulkan_wsi_kind::xlib)
		reshade::input::register_x11_window(hwnd, surface_info.display, surface_info.kind == vulkan_wsi_kind::xcb ? reshade::input::x11_display_kind::xcb : reshade::input::x11_display_kind::xlib, reinterpret_cast<uintptr_t>(create_info.surface), create_info.imageExtent.width, create_info.imageExtent.height);
#endif

	auto *const swapchain_impl = new reshade::vulkan::object_data<VK_OBJECT_TYPE_SWAPCHAIN_KHR>(device_impl, *pSwapchain, create_info, hwnd);
	reshade::create_effect_runtime(swapchain_impl, device_impl->_primary_graphics_queue);

	device_impl->register_object<VK_OBJECT_TYPE_SWAPCHAIN_KHR>(swapchain_impl->_orig, swapchain_impl);
	reshade::log::message(reshade::log::level::info, "Swapchain: handle=%p surface=%p oldSwapchain=%p extent=%ux%u presentMode=%d.", *pSwapchain, create_info.surface, create_info.oldSwapchain, create_info.imageExtent.width, create_info.imageExtent.height, static_cast<int>(create_info.presentMode));

	// Get back buffer images of new swap chain
	uint32_t num_images = 0;
	vk.GetSwapchainImagesKHR(device, swapchain_impl->_orig, &num_images, nullptr);
	temp_mem<VkImage, 3> swapchain_images(num_images);
	vk.GetSwapchainImagesKHR(device, swapchain_impl->_orig, &num_images, swapchain_images.p);

	// Add swap chain images to the image list
	for (uint32_t i = 0; i < num_images; ++i)
	{
		reshade::vulkan::object_data<VK_OBJECT_TYPE_IMAGE> &image_data = *device_impl->register_object<VK_OBJECT_TYPE_IMAGE>(swapchain_images[i]);
		image_data.create_info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
		image_data.create_info.imageType = VK_IMAGE_TYPE_2D;
		image_data.create_info.format = create_info.imageFormat;
		image_data.create_info.extent = { create_info.imageExtent.width, create_info.imageExtent.height, 1 };
		image_data.create_info.mipLevels = 1;
		image_data.create_info.arrayLayers = create_info.imageArrayLayers;
		image_data.create_info.samples = VK_SAMPLE_COUNT_1_BIT;
		image_data.create_info.usage = create_info.imageUsage;
		image_data.create_info.sharingMode = create_info.imageSharingMode;
		image_data.create_info.initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

		// See https://registry.khronos.org/vulkan/specs/latest/man/html/vkCreateSwapchainKHR.html#_description
		if ((create_info.flags & VK_SWAPCHAIN_CREATE_SPLIT_INSTANCE_BIND_REGIONS_BIT_KHR) != 0)
			image_data.create_info.flags |= VK_IMAGE_CREATE_SPLIT_INSTANCE_BIND_REGIONS_BIT;
		if ((create_info.flags & VK_SWAPCHAIN_CREATE_PROTECTED_BIT_KHR) != 0)
			image_data.create_info.flags |= VK_IMAGE_CREATE_PROTECTED_BIT;
#if VK_KHR_swapchain_mutable_format
		if ((create_info.flags & VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR) != 0)
			image_data.create_info.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
#endif
	}

	if (use_proxy_images)
	{
		reshade::api::resource_desc proxy_desc = {};
		proxy_desc.type = reshade::api::resource_type::texture_2d;
		proxy_desc.texture.width = application_create_info.imageExtent.width;
		proxy_desc.texture.height = application_create_info.imageExtent.height;
		assert(application_create_info.imageArrayLayers <= std::numeric_limits<uint16_t>::max());
		proxy_desc.texture.depth_or_layers = static_cast<uint16_t>(application_create_info.imageArrayLayers);
		proxy_desc.texture.levels = 1;
		proxy_desc.texture.format = reshade::vulkan::convert_format(application_create_info.imageFormat);
		proxy_desc.texture.samples = 1;
		proxy_desc.heap = reshade::api::memory_heap::default_;
		reshade::vulkan::convert_image_usage_flags_to_usage(application_create_info.imageUsage, proxy_desc.usage);
		proxy_desc.usage |= reshade::api::resource_usage::copy_source;

		reshade::api::resource_desc srgb_desc = proxy_desc;
		srgb_desc.texture.format = reshade::api::format_to_default_typed(proxy_desc.texture.format, 1);
		srgb_desc.usage = reshade::api::resource_usage::copy_source | reshade::api::resource_usage::copy_dest;

		swapchain_impl->_proxy_images.reserve(num_images);
		swapchain_impl->_proxy_srgb_images.reserve(num_images);
		swapchain_impl->_proxy_srgb_images_initialized.reserve(num_images);
		for (uint32_t i = 0; i < num_images; ++i)
		{
			reshade::api::resource image, srgb_image;
			if (!device_impl->create_resource(proxy_desc, nullptr, reshade::api::resource_usage::undefined, &image) ||
				!device_impl->create_resource(srgb_desc, nullptr, reshade::api::resource_usage::undefined, &srgb_image))
			{
				reshade::log::message(reshade::log::level::error, "Failed to create a Vulkan proxy swapchain image.");
				device_impl->destroy_resource(image);
				device_impl->destroy_resource(srgb_image);
				destroy_proxy_images(device_impl, swapchain_impl);
				break;
			}
			swapchain_impl->_proxy_images.push_back(image);
			swapchain_impl->_proxy_srgb_images.push_back(srgb_image);
			swapchain_impl->_proxy_srgb_images_initialized.push_back(false);
		}
	}

#if RESHADE_ADDON
	reshade::invoke_addon_event<reshade::addon_event::init_swapchain>(swapchain_impl, false);

	// Create default views for swap chain images (do this after the 'init_swapchain' event, so that the images are known to add-ons)
	for (uint32_t i = 0; i < num_images; ++i)
		create_default_view(device_impl, swapchain_images[i]);

#if VK_EXT_full_screen_exclusive
	if (fullscreen_info.fullScreenExclusive != VK_FULL_SCREEN_EXCLUSIVE_DEFAULT_EXT)
	{
		if (const auto fullscreen_win32_info = find_in_structure_chain<VkSurfaceFullScreenExclusiveWin32InfoEXT>(
				create_info.pNext, VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_WIN32_INFO_EXT))
			swapchain_impl->hmonitor = fullscreen_win32_info->hmonitor;

		reshade::invoke_addon_event<reshade::addon_event::set_fullscreen_state>(swapchain_impl, fullscreen_info.fullScreenExclusive == VK_FULL_SCREEN_EXCLUSIVE_ALLOWED_EXT, swapchain_impl->hmonitor);
	}
#endif
#endif

	reshade::init_effect_runtime(swapchain_impl);

#if RESHADE_VERBOSE_LOG
	reshade::log::message(reshade::log::level::debug, "Returning Vulkan swap chain %p.", *pSwapchain);
#endif
	return result;
}
void     VKAPI_CALL vkDestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks *pAllocator)
{
	reshade::log::message(reshade::log::level::info, "Redirecting vkDestroySwapchainKHR(device = %p, swapchain = %p, pAllocator = %p) ...", device, swapchain, pAllocator);

	if (swapchain == VK_NULL_HANDLE)
		return;

	reshade::vulkan::device_impl *const device_impl = g_vulkan_devices.at(dispatch_key_from_handle(device));
	RESHADE_VULKAN_GET_DEVICE_DISPATCH_PTR(DestroySwapchainKHR, device_impl);

	// Remove swap chain from global list
	reshade::vulkan::object_data<VK_OBJECT_TYPE_SWAPCHAIN_KHR> *const swapchain_impl = device_impl->get_private_data_for_object<VK_OBJECT_TYPE_SWAPCHAIN_KHR, true>(swapchain);
	if (swapchain_impl != nullptr)
	{
		if (!swapchain_impl->_runtime_destroyed)
		{
			reshade::reset_effect_runtime(swapchain_impl);

		#if RESHADE_ADDON
			reshade::invoke_addon_event<reshade::addon_event::destroy_swapchain>(swapchain_impl, false);
		#endif
			reshade::destroy_effect_runtime(swapchain_impl);
			swapchain_impl->_runtime_destroyed = true;
		}

		// Image/private-data cleanup is deliberately delayed until the application's destroy
		// call, because an old swapchain remains legal to present after recreation.
		uint32_t num_images = 0;
		vk.GetSwapchainImagesKHR(device, swapchain, &num_images, nullptr);
		temp_mem<VkImage, 3> swapchain_images(num_images);
		vk.GetSwapchainImagesKHR(device, swapchain, &num_images, swapchain_images.p);

		destroy_proxy_images(device_impl, swapchain_impl);

		for (uint32_t i = 0; i < num_images; ++i)
		{
#if RESHADE_ADDON
			destroy_default_view(device_impl, swapchain_images[i]);
#endif

			device_impl->unregister_object<VK_OBJECT_TYPE_IMAGE>(swapchain_images[i]);
		}

	}

	device_impl->unregister_object<VK_OBJECT_TYPE_SWAPCHAIN_KHR, false>(swapchain);

	delete swapchain_impl;

	trampoline(device, swapchain, pAllocator);
}

VkResult VKAPI_CALL vkGetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR swapchain, uint32_t *pSwapchainImageCount, VkImage *pSwapchainImages)
{
	reshade::vulkan::device_impl *const device_impl = g_vulkan_devices.at(dispatch_key_from_handle(device));
	RESHADE_VULKAN_GET_DEVICE_DISPATCH_PTR(GetSwapchainImagesKHR, device_impl);

	if (const auto swapchain_impl = device_impl->get_private_data_for_object<VK_OBJECT_TYPE_SWAPCHAIN_KHR, true>(swapchain);
		swapchain_impl != nullptr && !swapchain_impl->_proxy_images.empty())
	{
		assert(pSwapchainImageCount != nullptr);
		const uint32_t image_count = static_cast<uint32_t>(swapchain_impl->_proxy_images.size());
		if (pSwapchainImages == nullptr)
		{
			*pSwapchainImageCount = image_count;
			return VK_SUCCESS;
		}

		const uint32_t returned_count = std::min(*pSwapchainImageCount, image_count);
		for (uint32_t i = 0; i < returned_count; ++i)
			pSwapchainImages[i] = reinterpret_cast<VkImage>(swapchain_impl->_proxy_images[i].handle);
		*pSwapchainImageCount = returned_count;
		return returned_count == image_count ? VK_SUCCESS : VK_INCOMPLETE;
	}

	return trampoline(device, swapchain, pSwapchainImageCount, pSwapchainImages);
}

VkResult VKAPI_CALL vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pPresentInfo)
{
	assert(pPresentInfo != nullptr);

	VkPresentInfoKHR present_info = *pPresentInfo;

	reshade::vulkan::device_impl *const device_impl = g_vulkan_devices.at(dispatch_key_from_handle(queue));
	reshade::vulkan::object_data<VK_OBJECT_TYPE_QUEUE> *const queue_impl = device_impl->get_private_data_for_object<VK_OBJECT_TYPE_QUEUE>(queue);

	const bool present_from_secondary_queue = device_impl->_primary_graphics_queue != nullptr && device_impl->_primary_graphics_queue != queue_impl;
	if (present_from_secondary_queue)
		std::lock(queue_impl->_mutex, device_impl->_primary_graphics_queue->_mutex);
	else
		queue_impl->_mutex.lock();

	for (uint32_t i = 0; i < pPresentInfo->swapchainCount; ++i)
	{
		reshade::vulkan::object_data<VK_OBJECT_TYPE_SWAPCHAIN_KHR> *const swapchain_impl = device_impl->get_private_data_for_object<VK_OBJECT_TYPE_SWAPCHAIN_KHR, true>(pPresentInfo->pSwapchains[i]);
		if (swapchain_impl == nullptr || swapchain_impl->_retired)
		{
			static std::atomic_uint skipped_retired_present_count = 0;
			if (skipped_retired_present_count.fetch_add(1, std::memory_order_relaxed) < 8)
				reshade::log::message(reshade::log::level::warning, "Skipping ReShade processing for untracked or retired Vulkan swapchain %p.", pPresentInfo->pSwapchains[i]);
			continue;
		}

		// 'vkAcquireNextImageKHR' may be called for the next frame before this frame was presented (e.g. in DOOM Eternal), so correct swap index must be obtained from the present info
		swapchain_impl->_swap_index = pPresentInfo->pImageIndices[i];

		if (!swapchain_impl->_proxy_images.empty())
		{
			const uint32_t image_index = pPresentInfo->pImageIndices[i];
			if (image_index >= swapchain_impl->_proxy_images.size() || image_index >= swapchain_impl->_proxy_srgb_images.size() || device_impl->_primary_graphics_queue == nullptr)
			{
				reshade::log::message(reshade::log::level::error, "Cannot copy a Vulkan proxy swapchain image for presentation.");
			}
			else if (auto *const command_list = device_impl->_primary_graphics_queue->get_immediate_command_list())
			{
				const reshade::api::resource source = swapchain_impl->_proxy_images[image_index];
				const reshade::api::resource srgb_source = swapchain_impl->_proxy_srgb_images[image_index];
				const reshade::api::resource destination = swapchain_impl->get_back_buffer(image_index);
				command_list->barrier(source, reshade::api::resource_usage::present, reshade::api::resource_usage::copy_source);
				command_list->barrier(srgb_source,
					swapchain_impl->_proxy_srgb_images_initialized[image_index] ? reshade::api::resource_usage::copy_dest : reshade::api::resource_usage::undefined,
					reshade::api::resource_usage::copy_dest);
				command_list->copy_texture_region(source, 0, nullptr, srgb_source, 0, nullptr, reshade::api::filter_mode::min_mag_mip_point);
				command_list->barrier(destination, reshade::api::resource_usage::present, reshade::api::resource_usage::copy_dest);

				// A non-null destination box forces a blit even when the formats have the
				// same byte size (e.g. BGRA8 to RGB10A2). This applies the sRGB decode
				// before the final HDR format conversion rather than copying raw bits.
				const reshade::api::subresource_box destination_box = { 0, 0, 0, swapchain_impl->_create_info.imageExtent.width, swapchain_impl->_create_info.imageExtent.height, 1 };
				command_list->barrier(srgb_source, reshade::api::resource_usage::copy_dest, reshade::api::resource_usage::copy_source);
				command_list->copy_texture_region(srgb_source, 0, nullptr, destination, 0, &destination_box, reshade::api::filter_mode::min_mag_mip_linear);
				command_list->barrier(source, reshade::api::resource_usage::copy_source, reshade::api::resource_usage::present);
				command_list->barrier(srgb_source, reshade::api::resource_usage::copy_source, reshade::api::resource_usage::copy_dest);
				command_list->barrier(destination, reshade::api::resource_usage::copy_dest, reshade::api::resource_usage::present);
				swapchain_impl->_proxy_srgb_images_initialized[image_index] = true;
			}
		}

#if RESHADE_ADDON
#if VK_KHR_incremental_present
		uint32_t dirty_rect_count = 0;
		temp_mem<reshade::api::rect, 16> dirty_rects;

		const auto present_regions = find_in_structure_chain<VkPresentRegionsKHR>(pPresentInfo->pNext, VK_STRUCTURE_TYPE_PRESENT_REGIONS_KHR);
		if (present_regions != nullptr)
		{
			assert(present_regions->swapchainCount == pPresentInfo->swapchainCount);

			dirty_rect_count = present_regions->pRegions[i].rectangleCount;
			if (dirty_rect_count > 16)
				dirty_rects.p = new reshade::api::rect[dirty_rect_count];

			const VkRectLayerKHR *const rects = present_regions->pRegions[i].pRectangles;

			for (uint32_t k = 0; k < dirty_rect_count; ++k)
			{
				dirty_rects[k] = {
					rects[k].offset.x,
					rects[k].offset.y,
					rects[k].offset.x + static_cast<int32_t>(rects[k].extent.width),
					rects[k].offset.y + static_cast<int32_t>(rects[k].extent.height)
				};
			}
		}
#endif

#if VK_KHR_display_swapchain
		reshade::api::rect source_rect, dest_rect;

		const auto display_present_info = find_in_structure_chain<VkDisplayPresentInfoKHR>(pPresentInfo->pNext, VK_STRUCTURE_TYPE_DISPLAY_PRESENT_INFO_KHR);
		if (display_present_info != nullptr)
		{
			source_rect = {
				display_present_info->srcRect.offset.x,
				display_present_info->srcRect.offset.y,
				display_present_info->srcRect.offset.x + static_cast<int32_t>(display_present_info->srcRect.extent.width),
				display_present_info->srcRect.offset.y + static_cast<int32_t>(display_present_info->srcRect.extent.height)
			};
			dest_rect = {
				display_present_info->dstRect.offset.x,
				display_present_info->dstRect.offset.y,
				display_present_info->dstRect.offset.x + static_cast<int32_t>(display_present_info->dstRect.extent.width),
				display_present_info->dstRect.offset.y + static_cast<int32_t>(display_present_info->dstRect.extent.height)
			};
		}

		reshade::invoke_addon_event<reshade::addon_event::present>(
			queue_impl,
			swapchain_impl,
			display_present_info != nullptr ? &source_rect : nullptr,
			display_present_info != nullptr ? &dest_rect : nullptr,
#else
		reshade::invoke_addon_event<reshade::addon_event::present>(
			queue_impl,
			swapchain_impl,
			nullptr,
			nullptr,
#endif
#if VK_KHR_incremental_present
			dirty_rect_count,
			dirty_rect_count != 0 ? dirty_rects.p : nullptr);
#else
			0, nullptr);
#endif
#endif

		reshade::present_effect_runtime(swapchain_impl);
	}

	// Synchronize immediate command list flush
	{
		temp_mem<VkPipelineStageFlags> wait_stages(present_info.waitSemaphoreCount);
		std::fill_n(wait_stages.p, present_info.waitSemaphoreCount, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);

		VkSubmitInfo submit_info { VK_STRUCTURE_TYPE_SUBMIT_INFO };
		submit_info.waitSemaphoreCount = present_info.waitSemaphoreCount;
		submit_info.pWaitSemaphores = present_info.pWaitSemaphores;
		submit_info.pWaitDstStageMask = wait_stages.p;

		queue_impl->flush_immediate_command_list(&submit_info);

		// If the application is presenting with a different queue than rendering, synchronize these two queues
		if (present_from_secondary_queue)
		{
			if (const auto present_config = find_in_structure_chain<VkBaseInStructure>(
					pPresentInfo, static_cast<VkStructureType>(1000613000) /* VK_STRUCTURE_TYPE_SET_PRESENT_CONFIG_NV */))
			{
				assert(queue_impl->present_batch <= 1);
				queue_impl->present_batch = *reinterpret_cast<const uint32_t *>(present_config + 1);
			}
			else if (queue_impl->present_batch > 1)
			{
				queue_impl->present_batch--;

				if (submit_info.waitSemaphoreCount != 0)
				{
					// RTX Remix calls present with the same semaphores to wait on for all generated frames, which will already be signaled by the first present in the batch
					// Signal them on the present queue again, so that the graphics queue waits for the generated frames too
					submit_info.signalSemaphoreCount = submit_info.waitSemaphoreCount;
					submit_info.pSignalSemaphores = submit_info.pWaitSemaphores;
					vk.QueueSubmit(queue, 1, &submit_info, VK_NULL_HANDLE);
					submit_info.signalSemaphoreCount = 0;
					submit_info.pSignalSemaphores = nullptr;
				}
			}
			else if (submit_info.pWaitSemaphores == present_info.pWaitSemaphores)
			{
				// Games doing present from compute may process the swap chain image on the compute queue before presenting, so ensure this work has completed before executing work on the graphics queue
				queue_impl->wait_and_signal(&submit_info);
			}

			// This can deadlock on the GPU if the application submitted a semaphore wait to the graphics queue before this call, for which it submits the corresponding signal to the present queue only after this call
			// E.g. happens with DLSS Frame Generation
			device_impl->_primary_graphics_queue->flush_immediate_command_list(&submit_info);
		}

		// Override wait semaphores based on the last queue submit
		present_info.waitSemaphoreCount = submit_info.waitSemaphoreCount;
		present_info.pWaitSemaphores = submit_info.pWaitSemaphores;
	}

	RESHADE_VULKAN_GET_DEVICE_DISPATCH_PTR(QueuePresentKHR, device_impl);
	assert(!g_in_dxgi_runtime);
	g_in_dxgi_runtime = true;
	const VkResult result = trampoline(queue, &present_info);
	g_in_dxgi_runtime = false;

#if RESHADE_ADDON
	if (result >= VK_SUCCESS && reshade::has_addon_event<reshade::addon_event::finish_present>())
	{
		for (uint32_t i = 0; i < pPresentInfo->swapchainCount; ++i)
		{
			if (reshade::vulkan::object_data<VK_OBJECT_TYPE_SWAPCHAIN_KHR> *const swapchain_impl = device_impl->get_private_data_for_object<VK_OBJECT_TYPE_SWAPCHAIN_KHR, true>(pPresentInfo->pSwapchains[i]); swapchain_impl != nullptr && !swapchain_impl->_retired)
				reshade::invoke_addon_event<reshade::addon_event::finish_present>(queue_impl, swapchain_impl);
		}
	}
#endif

	if (present_from_secondary_queue)
		device_impl->_primary_graphics_queue->_mutex.unlock();
	queue_impl->_mutex.unlock();

	return result;
}
#endif

#if VK_EXT_full_screen_exclusive
VkResult VKAPI_CALL vkAcquireFullScreenExclusiveModeEXT(VkDevice device, VkSwapchainKHR swapchain)
{
	reshade::vulkan::device_impl *const device_impl = g_vulkan_devices.at(dispatch_key_from_handle(device));
	RESHADE_VULKAN_GET_DEVICE_DISPATCH_PTR(AcquireFullScreenExclusiveModeEXT, device_impl);

#if RESHADE_ADDON
	if (reshade::vulkan::object_data<VK_OBJECT_TYPE_SWAPCHAIN_KHR> *const swapchain_impl = device_impl->get_private_data_for_object<VK_OBJECT_TYPE_SWAPCHAIN_KHR, true>(swapchain))
		if (reshade::invoke_addon_event<reshade::addon_event::set_fullscreen_state>(swapchain_impl, true, swapchain_impl->hmonitor))
			return VK_SUCCESS;
#endif

	return trampoline(device, swapchain);
}
VkResult VKAPI_CALL vkReleaseFullScreenExclusiveModeEXT(VkDevice device, VkSwapchainKHR swapchain)
{
	reshade::vulkan::device_impl *const device_impl = g_vulkan_devices.at(dispatch_key_from_handle(device));
	RESHADE_VULKAN_GET_DEVICE_DISPATCH_PTR(ReleaseFullScreenExclusiveModeEXT, device_impl);

#if RESHADE_ADDON
	if (reshade::vulkan::object_data<VK_OBJECT_TYPE_SWAPCHAIN_KHR> *const swapchain_impl = device_impl->get_private_data_for_object<VK_OBJECT_TYPE_SWAPCHAIN_KHR, true>(swapchain))
		if (reshade::invoke_addon_event<reshade::addon_event::set_fullscreen_state>(swapchain_impl, false, swapchain_impl->hmonitor))
			return VK_SUCCESS;
#endif

	return trampoline(device, swapchain);
}
#endif
