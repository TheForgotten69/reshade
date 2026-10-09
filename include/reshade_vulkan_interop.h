
#pragma once


#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void ReShadeVulkanRequestDeviceExtension(const char *name);

void ReShadeVulkanHideDeviceExtension(const char *name);

uint32_t ReShadeVulkanRequestQueue(uint32_t queue_flags);

struct ReShadeVulkanQueue
{
	void *queue;
	uint32_t family_index;
};

struct ReShadeVulkanDeviceInterop
{
	uint32_t size;
	void *instance;
	void *physical_device;
	void *(*get_instance_proc_addr)(void *instance, const char *name);
	void *(*get_device_proc_addr)(void *device, const char *name);
	uint32_t api_version;
	uint32_t enabled_extension_count;
	const char *const *enabled_extensions;
	uint32_t buffer_device_address;
	uint32_t queue_count;
	const struct ReShadeVulkanQueue *queues;
	uint32_t queue_family_count;
	const uint32_t *queue_family_indices;
};

uint32_t ReShadeVulkanGetDeviceInterop(void *device, struct ReShadeVulkanDeviceInterop *out);

void *ReShadeVulkanGetMappedBuffer(void *device, uint64_t buffer);

const void *ReShadeVulkanInterruptRendering(void *command_buffer);
void ReShadeVulkanResumeRendering(void *command_buffer);

void ReShadeVulkanRestoreState(void *command_buffer);

void ReShadeVulkanObserveGraphicsPipelines(void (*observer)(void *device, const void *create_info, uint64_t pipeline, void *user_data), void *user_data);

const uint32_t *ReShadeVulkanGetShaderModuleCode(void *device, uint64_t shader_module, uint32_t *word_count);

#ifdef __cplusplus
}
#endif
