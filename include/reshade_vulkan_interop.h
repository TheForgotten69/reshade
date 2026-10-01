/*
 * Copyright (C) 2014 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause OR MIT
 */

#pragma once

// Linux Vulkan layer only: lets an add-on drive the game's VkDevice with native Vulkan calls (for
// example a library that records its own commands), beyond what the ReShade API covers.
// Vulkan types are passed as void pointers so that this header does not need the Vulkan headers.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Asks ReShade to enable a device extension on devices created from now on, when the driver supports it.
/// Call this from AddonInit: add-ons are loaded when the instance is created, before its devices.
/// Requesting "VK_KHR_buffer_device_address" also enables the bufferDeviceAddress feature (core since Vulkan 1.2).
void ReShadeVulkanRequestDeviceExtension(const char *name);

struct ReShadeVulkanDeviceInterop
{
	uint32_t size; ///< Set by the caller to sizeof(ReShadeVulkanDeviceInterop)
	void *instance; ///< VkInstance
	void *physical_device; ///< VkPhysicalDevice
	/// The next layer's vkGetInstanceProcAddr and vkGetDeviceProcAddr: calls through these do not come back through ReShade.
	void *(*get_instance_proc_addr)(void *instance, const char *name);
	void *(*get_device_proc_addr)(void *device, const char *name);
	uint32_t api_version; ///< The instance's Vulkan API version
	uint32_t enabled_extension_count;
	const char *const *enabled_extensions; ///< Valid until the device is destroyed
	uint32_t buffer_device_address; ///< Whether the bufferDeviceAddress feature is enabled
};

/// Describes a device ReShade hooked (the VkDevice of api::device::get_native()). Returns 0 for any other device.
uint32_t ReShadeVulkanGetDeviceInterop(void *device, struct ReShadeVulkanDeviceInterop *out);

#ifdef __cplusplus
}
#endif
