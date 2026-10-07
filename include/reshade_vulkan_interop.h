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
/// Requesting "VK_KHR_buffer_device_address" also enables the bufferDeviceAddress feature (core since Vulkan 1.2),
/// and "VK_NV_optical_flow" the opticalFlow feature (the extension is only enabled when the feature is supported).
void ReShadeVulkanRequestDeviceExtension(const char *name);

/// Hides a device extension from the application on every Vulkan device created afterwards, so that it
/// uses the paths without it. For add-ons that follow what the application draws through calls that a
/// newer extension replaces (e.g. "VK_EXT_descriptor_heap" replaces descriptor sets). Call from the
/// add-on's initialization.
void ReShadeVulkanHideDeviceExtension(const char *name);

/// Asks ReShade to give devices created from now on one more queue with at least 'queue_flags' (VkQueueFlags),
/// for the add-on's own work. The family with the fewest other capabilities is preferred, so that the work can run
/// beside the game's. The game never sees this queue. Call this from AddonInit.
/// Returns the queue's index in ReShadeVulkanDeviceInterop::queues; asking again for the same flags returns the same index.
uint32_t ReShadeVulkanRequestQueue(uint32_t queue_flags);

struct ReShadeVulkanQueue
{
	void *queue; ///< VkQueue, or null when no family had room
	uint32_t family_index;
};

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
	// Filled only when 'size' covers them:
	uint32_t queue_count;
	const struct ReShadeVulkanQueue *queues; ///< Added for ReShadeVulkanRequestQueue, in request order; valid until the device is destroyed
	uint32_t queue_family_count; ///< The queue families the device was created with (for concurrent sharing)
	const uint32_t *queue_family_indices; ///< Valid until the device is destroyed
};

/// Describes a device ReShade hooked (the VkDevice of api::device::get_native()). Returns 0 for any other device.
uint32_t ReShadeVulkanGetDeviceInterop(void *device, struct ReShadeVulkanDeviceInterop *out);

#ifdef __cplusplus
}
#endif
