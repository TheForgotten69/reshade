/*
 * Copyright (C) 2014 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "vulkan_impl_device.hpp"
#include "vulkan_impl_swapchain.hpp"
#include "vulkan_impl_type_convert.hpp"

#define vk _device->_dispatch_table

reshade::vulkan::swapchain_impl::swapchain_impl(device_impl *device, VkSwapchainKHR swapchain, const VkSwapchainCreateInfoKHR &create_info, void *native_window) :
	api_object_impl(swapchain),
	_device(device),
	_create_info(create_info),
	_hwnd(native_window)
{
	_create_info.pNext = nullptr;

	assert(create_info.imageUsage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);

	uint32_t num_images = 0;
	if (vk.GetSwapchainImagesKHR(_device->_orig, _orig, &num_images, nullptr) == VK_SUCCESS && num_images != 0)
	{
		_images.resize(num_images);
		if (vk.GetSwapchainImagesKHR(_device->_orig, _orig, &num_images, _images.data()) < VK_SUCCESS)
			_images.clear();
	}
}

reshade::api::device *reshade::vulkan::swapchain_impl::get_device()
{
	return _device;
}

void *reshade::vulkan::swapchain_impl::get_hwnd() const
{
	return _hwnd;
}

reshade::api::resource reshade::vulkan::swapchain_impl::get_back_buffer(uint32_t index)
{
	return index < _images.size() ? api::resource { (uint64_t)_images[index] } : api::resource {};
}

uint32_t reshade::vulkan::swapchain_impl::get_back_buffer_count() const
{
	return static_cast<uint32_t>(_images.size());
}

uint32_t reshade::vulkan::swapchain_impl::get_current_back_buffer_index() const
{
	return _swap_index;
}

bool reshade::vulkan::swapchain_impl::check_color_space_support(api::color_space color_space) const
{
	uint32_t num_formats = 0;
	vk.GetPhysicalDeviceSurfaceFormatsKHR(_device->_physical_device, _create_info.surface, &num_formats, nullptr);
	temp_mem<VkSurfaceFormatKHR> formats(num_formats);
	vk.GetPhysicalDeviceSurfaceFormatsKHR(_device->_physical_device, _create_info.surface, &num_formats, formats.p);

	for (uint32_t i = 0; i < num_formats; ++i)
		if (formats[i].format == _create_info.imageFormat && formats[i].colorSpace == convert_color_space(color_space))
			return true;

	return false;
}

reshade::api::color_space reshade::vulkan::swapchain_impl::get_color_space() const
{
	return convert_color_space(_create_info.imageColorSpace);
}
