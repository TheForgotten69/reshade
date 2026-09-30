/*
 * Copyright (C) 2014 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include <vector>

namespace reshade::vulkan
{
	class device_impl;

	class swapchain_impl : public api::api_object_impl<VkSwapchainKHR, api::swapchain>
	{
	public:
		swapchain_impl(device_impl *device, VkSwapchainKHR swapchain, const VkSwapchainCreateInfoKHR &create_info, void *native_window);

		api::device *get_device() final;

		void *get_hwnd() const final;

		api::resource get_back_buffer(uint32_t index) final;

		uint32_t get_back_buffer_count() const final;
		uint32_t get_current_back_buffer_index() const final;

		bool check_color_space_support(api::color_space color_space) const final;

		api::color_space get_color_space() const final;

	protected:
		device_impl *const _device;

		VkSwapchainCreateInfoKHR _create_info = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
		void *_hwnd = nullptr;
		uint32_t _swap_index = 0;
		bool _retired = false;
		bool _runtime_destroyed = false;

		// When an add-on requests a presentation format different from the format
		// exposed to the application, the application renders into these ordinary
		// images while '_orig' remains the WSI swap chain used by ReShade.
		std::vector<api::resource> _proxy_images;
		std::vector<api::resource> _proxy_srgb_images;
		std::vector<bool> _proxy_srgb_images_initialized;
	};

	template <>
	struct object_data<VK_OBJECT_TYPE_SWAPCHAIN_KHR> : public swapchain_impl
	{
		using Handle = VkSwapchainKHR;

		object_data(device_impl *device, VkSwapchainKHR swapchain, const VkSwapchainCreateInfoKHR &create_info, void *native_window) :
			swapchain_impl(device, swapchain, create_info, native_window) {}

		using swapchain_impl::_create_info;
		using swapchain_impl::_hwnd;
		using swapchain_impl::_proxy_images;
		using swapchain_impl::_proxy_srgb_images;
		using swapchain_impl::_proxy_srgb_images_initialized;
		using swapchain_impl::_swap_index;
		using swapchain_impl::_retired;
		using swapchain_impl::_runtime_destroyed;

#if VK_EXT_full_screen_exclusive && defined(_WIN32)
		HMONITOR hmonitor = nullptr;
#endif
	};
}
