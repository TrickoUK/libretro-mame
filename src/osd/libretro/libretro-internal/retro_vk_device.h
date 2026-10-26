// license:BSD-3-Clause
// copyright-holders:TrickoUK
/***************************************************************************

    retro_vk_device.h

    The private Vulkan device behind the host-GPU engine: a headless
    instance, one logical device with one graphics queue, a command pool,
    fences and memory allocation. Nothing here touches RetroArch's own
    Vulkan instance or device.

    The device exists for the whole session once it has been created. A
    failure latches (failed()) and the caller stays on the software or
    OpenGL path for the rest of the session, as the GL engine does.

***************************************************************************/
#ifndef MAME_OSD_LIBRETRO_RETRO_VK_DEVICE_H
#define MAME_OSD_LIBRETRO_RETRO_VK_DEVICE_H

#pragma once

#define VK_NO_PROTOTYPES
#include "../../../../3rdparty/bgfx/3rdparty/khronos/vulkan-local/vulkan_core.h"

#include <cstdint>
#include <string>

namespace retro_vk {

// Entry points used by the device. Instance-level functions come from
// vkGetInstanceProcAddr, device-level ones from vkGetDeviceProcAddr.
struct vk_functions
{
	PFN_vkGetInstanceProcAddr get_instance_proc_addr = nullptr;
	PFN_vkEnumerateInstanceVersion enumerate_instance_version = nullptr;
	PFN_vkCreateInstance create_instance = nullptr;
	PFN_vkDestroyInstance destroy_instance = nullptr;
	PFN_vkEnumeratePhysicalDevices enumerate_physical_devices = nullptr;
	PFN_vkGetPhysicalDeviceProperties2 get_physical_device_properties2 = nullptr;
	PFN_vkGetPhysicalDeviceFeatures2 get_physical_device_features2 = nullptr;
	PFN_vkGetPhysicalDeviceQueueFamilyProperties get_queue_family_properties = nullptr;
	PFN_vkGetPhysicalDeviceMemoryProperties get_memory_properties = nullptr;
	PFN_vkCreateDevice create_device = nullptr;
	PFN_vkGetDeviceProcAddr get_device_proc_addr = nullptr;
	PFN_vkDestroyDevice destroy_device = nullptr;
	PFN_vkGetDeviceQueue get_device_queue = nullptr;
	PFN_vkDeviceWaitIdle device_wait_idle = nullptr;
	PFN_vkCreateCommandPool create_command_pool = nullptr;
	PFN_vkDestroyCommandPool destroy_command_pool = nullptr;
	PFN_vkResetCommandPool reset_command_pool = nullptr;
	PFN_vkAllocateCommandBuffers allocate_command_buffers = nullptr;
	PFN_vkFreeCommandBuffers free_command_buffers = nullptr;
	PFN_vkBeginCommandBuffer begin_command_buffer = nullptr;
	PFN_vkEndCommandBuffer end_command_buffer = nullptr;
	PFN_vkQueueSubmit queue_submit = nullptr;
	PFN_vkQueueWaitIdle queue_wait_idle = nullptr;
	PFN_vkCreateFence create_fence = nullptr;
	PFN_vkDestroyFence destroy_fence = nullptr;
	PFN_vkWaitForFences wait_for_fences = nullptr;
	PFN_vkResetFences reset_fences = nullptr;
	PFN_vkGetFenceStatus get_fence_status = nullptr;
	PFN_vkAllocateMemory allocate_memory = nullptr;
	PFN_vkFreeMemory free_memory = nullptr;
	PFN_vkMapMemory map_memory = nullptr;
	PFN_vkUnmapMemory unmap_memory = nullptr;
	PFN_vkFlushMappedMemoryRanges flush_mapped_memory_ranges = nullptr;
	PFN_vkInvalidateMappedMemoryRanges invalidate_mapped_memory_ranges = nullptr;
	PFN_vkCreateBuffer create_buffer = nullptr;
	PFN_vkDestroyBuffer destroy_buffer = nullptr;
	PFN_vkGetBufferMemoryRequirements get_buffer_memory_requirements = nullptr;
	PFN_vkBindBufferMemory bind_buffer_memory = nullptr;
};

class vk_device
{
public:
	vk_device() = default;
	~vk_device();

	vk_device(const vk_device &) = delete;
	vk_device &operator=(const vk_device &) = delete;

	// Creates the instance and device. Returns false, with the reason in
	// report, when no usable device exists or a call fails.
	bool create(std::string &report);
	void destroy();

	bool valid() const { return m_device != VK_NULL_HANDLE; }

	// Latches a failure: the device is not used again this session. Logs the
	// reason once.
	void latch(const char *what, const char *detail);
	bool failed() const { return m_failed; }

	// Memory type index with every flag in want and none of the flags in
	// excluded, among the types allowed by type_bits. Returns UINT32_MAX when
	// there is none.
	uint32_t find_memory_type(uint32_t type_bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags excluded = 0) const;

	// Device memory for the requirements, or VK_NULL_HANDLE when no memory type
	// fits or the allocation fails. The caller decides whether that latches.
	VkDeviceMemory allocate(const VkMemoryRequirements &req, VkMemoryPropertyFlags want, VkMemoryPropertyFlags excluded = 0);

	vk_functions fn;

	VkInstance instance() const { return m_instance; }
	VkPhysicalDevice physical_device() const { return m_physical; }
	VkDevice device() const { return m_device; }
	VkQueue queue() const { return m_queue; }
	VkCommandPool command_pool() const { return m_pool; }
	uint32_t queue_family() const { return m_queue_family; }
	bool dual_src_blend() const { return m_dual_src_blend; }
	const VkPhysicalDeviceProperties &properties() const { return m_props; }

private:
	bool load_instance_functions(std::string &report);

	void *m_library = nullptr;
	VkInstance m_instance = VK_NULL_HANDLE;
	VkPhysicalDevice m_physical = VK_NULL_HANDLE;
	VkDevice m_device = VK_NULL_HANDLE;
	VkQueue m_queue = VK_NULL_HANDLE;
	VkCommandPool m_pool = VK_NULL_HANDLE;
	uint32_t m_queue_family = 0;
	bool m_dual_src_blend = false;
	VkPhysicalDeviceProperties m_props = {};
	VkPhysicalDeviceMemoryProperties m_memory = {};
	bool m_failed = false;
};

} // namespace retro_vk

#endif // MAME_OSD_LIBRETRO_RETRO_VK_DEVICE_H
