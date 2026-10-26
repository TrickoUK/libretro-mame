// license:BSD-3-Clause
// copyright-holders:TrickoUK
/***************************************************************************

    retro_vk_device.cpp

    Headless Vulkan instance and device for the host-GPU engine (see
    retro_vk_device.h).

***************************************************************************/

#include "retro_vk_device.h"

#include <dlfcn.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace retro_vk {

namespace {

// Vulkan 1.3, VK_MAKE_API_VERSION(0, 1, 3, 0): the minimum the engine
// relies on (gpu-vulkan-plan.md decision 8.4).
constexpr uint32_t REQUIRED_API_VERSION = 0x00403000;

// Preference order for the physical device. The CPU (software) implementation
// is only used when named by MAME_GPU_VK_DEVICE, which is useful for lavapipe.
int type_rank(VkPhysicalDeviceType type)
{
	switch (type)
	{
	case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return 0;
	case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 1;
	case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return 2;
	case VK_PHYSICAL_DEVICE_TYPE_OTHER:          return 3;
	default:                                     return -1;
	}
}

const char *type_name(VkPhysicalDeviceType type)
{
	switch (type)
	{
	case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return "discrete";
	case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated";
	case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return "virtual";
	case VK_PHYSICAL_DEVICE_TYPE_CPU:            return "cpu";
	default:                                     return "other";
	}
}

std::string api_string(uint32_t version)
{
	return std::to_string(VK_API_VERSION_MAJOR(version)) + "." + std::to_string(VK_API_VERSION_MINOR(version)) + "." +
			std::to_string(VK_API_VERSION_PATCH(version));
}

// Typed lookups: instance-level and device-level entry points by name.
template <typename F>
F instance_proc(PFN_vkGetInstanceProcAddr gpa, VkInstance instance, const char *name)
{
	return reinterpret_cast<F>(gpa(instance, name));
}

template <typename F>
F device_proc(PFN_vkGetDeviceProcAddr gpa, VkDevice device, const char *name)
{
	return reinterpret_cast<F>(gpa(device, name));
}

// The first queue family with graphics support, or UINT32_MAX.
uint32_t graphics_queue_family(const vk_functions &fn, VkPhysicalDevice device)
{
	uint32_t count = 0;
	fn.get_queue_family_properties(device, &count, nullptr);
	std::vector<VkQueueFamilyProperties> families(count);
	fn.get_queue_family_properties(device, &count, families.data());
	for (uint32_t i = 0; i < count; i++)
		if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
			return i;
	return UINT32_MAX;
}

} // anonymous namespace

vk_device::~vk_device()
{
	destroy();
}

bool vk_device::load_instance_functions(std::string &report)
{
	m_library = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
	if (!m_library)
	{
		report = "libvulkan.so.1 not found";
		return false;
	}
	// The library stays loaded for the rest of the process: some ICDs misbehave when unloaded.
	fn.get_instance_proc_addr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(m_library, "vkGetInstanceProcAddr"));
	if (!fn.get_instance_proc_addr)
	{
		report = "libvulkan.so.1 has no vkGetInstanceProcAddr";
		return false;
	}
	fn.enumerate_instance_version = instance_proc<PFN_vkEnumerateInstanceVersion>(fn.get_instance_proc_addr, nullptr, "vkEnumerateInstanceVersion");
	fn.create_instance = instance_proc<PFN_vkCreateInstance>(fn.get_instance_proc_addr, nullptr, "vkCreateInstance");
	if (!fn.enumerate_instance_version || !fn.create_instance)
	{
		report = "Vulkan loader is older than 1.1";
		return false;
	}
	uint32_t loader_version = 0;
	fn.enumerate_instance_version(&loader_version);
	if (loader_version < REQUIRED_API_VERSION)
	{
		report = "Vulkan loader is " + api_string(loader_version) + ", need 1.3";
		return false;
	}
	return true;
}

bool vk_device::create(std::string &report)
{
	if (!load_instance_functions(report))
		return false;

	VkApplicationInfo app_info = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
	app_info.pApplicationName = "MAME (libretro)";
	app_info.apiVersion = REQUIRED_API_VERSION;
	VkInstanceCreateInfo instance_info = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
	instance_info.pApplicationInfo = &app_info;
	// headless: no surface extensions and no layers (validation is for debug sessions)
	if (fn.create_instance(&instance_info, nullptr, &m_instance) != VK_SUCCESS)
	{
		report = "vkCreateInstance failed";
		return false;
	}

	fn.destroy_instance = instance_proc<PFN_vkDestroyInstance>(fn.get_instance_proc_addr, m_instance, "vkDestroyInstance");
	fn.enumerate_physical_devices = instance_proc<PFN_vkEnumeratePhysicalDevices>(fn.get_instance_proc_addr, m_instance, "vkEnumeratePhysicalDevices");
	fn.get_physical_device_properties2 = instance_proc<PFN_vkGetPhysicalDeviceProperties2>(fn.get_instance_proc_addr, m_instance, "vkGetPhysicalDeviceProperties2");
	fn.get_physical_device_features2 = instance_proc<PFN_vkGetPhysicalDeviceFeatures2>(fn.get_instance_proc_addr, m_instance, "vkGetPhysicalDeviceFeatures2");
	fn.get_queue_family_properties = instance_proc<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(fn.get_instance_proc_addr, m_instance, "vkGetPhysicalDeviceQueueFamilyProperties");
	fn.get_memory_properties = instance_proc<PFN_vkGetPhysicalDeviceMemoryProperties>(fn.get_instance_proc_addr, m_instance, "vkGetPhysicalDeviceMemoryProperties");
	fn.create_device = instance_proc<PFN_vkCreateDevice>(fn.get_instance_proc_addr, m_instance, "vkCreateDevice");
	fn.get_device_proc_addr = instance_proc<PFN_vkGetDeviceProcAddr>(fn.get_instance_proc_addr, m_instance, "vkGetDeviceProcAddr");

	uint32_t device_count = 0;
	fn.enumerate_physical_devices(m_instance, &device_count, nullptr);
	std::vector<VkPhysicalDevice> devices(device_count);
	if (device_count)
		fn.enumerate_physical_devices(m_instance, &device_count, devices.data());

	// MAME_GPU_VK_DEVICE: an index into the list, or a substring of the device name
	const char *const wanted = std::getenv("MAME_GPU_VK_DEVICE");
	int best_rank = 1000;
	uint32_t best_family = UINT32_MAX;
	VkPhysicalDeviceFeatures2 best_features = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
	for (uint32_t i = 0; i < device_count; i++)
	{
		VkPhysicalDeviceProperties2 props = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
		fn.get_physical_device_properties2(devices[i], &props);
		if (wanted && strcmp(wanted, std::to_string(i).c_str()) != 0 && !strstr(props.properties.deviceName, wanted))
			continue;
		if (props.properties.apiVersion < REQUIRED_API_VERSION)
			continue;
		const uint32_t family = graphics_queue_family(fn, devices[i]);
		if (family == UINT32_MAX)
			continue;
		// the CPU implementation is only used when named explicitly
		const int rank = (props.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) ? (wanted ? 0 : -1) : type_rank(props.properties.deviceType);
		if (rank < 0 || (m_physical && rank >= best_rank))
			continue;
		m_physical = devices[i];
		m_props = props.properties;
		best_rank = rank;
		best_family = family;
		best_features = VkPhysicalDeviceFeatures2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
		fn.get_physical_device_features2(devices[i], &best_features);
	}
	if (!m_physical)
	{
		report = wanted ? std::string("no usable device matches MAME_GPU_VK_DEVICE=") + wanted
				: std::string("no device with Vulkan 1.3 and a graphics queue");
		return false;
	}
	m_queue_family = best_family;
	m_dual_src_blend = best_features.features.dualSrcBlend;
	fn.get_memory_properties(m_physical, &m_memory);

	// one graphics queue, dual-source blending enabled when the device has it (System 22)
	const float priority = 1.0f;
	VkDeviceQueueCreateInfo queue_info = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
	queue_info.queueFamilyIndex = m_queue_family;
	queue_info.queueCount = 1;
	queue_info.pQueuePriorities = &priority;
	VkPhysicalDeviceFeatures features = {};
	features.dualSrcBlend = m_dual_src_blend ? VK_TRUE : VK_FALSE;
	VkDeviceCreateInfo device_info = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
	device_info.queueCreateInfoCount = 1;
	device_info.pQueueCreateInfos = &queue_info;
	device_info.pEnabledFeatures = &features;
	if (fn.create_device(m_physical, &device_info, nullptr, &m_device) != VK_SUCCESS)
	{
		report = "vkCreateDevice failed";
		return false;
	}

	// device-level entry points
	const auto dp = fn.get_device_proc_addr;
	fn.destroy_device = device_proc<PFN_vkDestroyDevice>(dp, m_device, "vkDestroyDevice");
	fn.get_device_queue = device_proc<PFN_vkGetDeviceQueue>(dp, m_device, "vkGetDeviceQueue");
	fn.device_wait_idle = device_proc<PFN_vkDeviceWaitIdle>(dp, m_device, "vkDeviceWaitIdle");
	fn.create_command_pool = device_proc<PFN_vkCreateCommandPool>(dp, m_device, "vkCreateCommandPool");
	fn.destroy_command_pool = device_proc<PFN_vkDestroyCommandPool>(dp, m_device, "vkDestroyCommandPool");
	fn.reset_command_pool = device_proc<PFN_vkResetCommandPool>(dp, m_device, "vkResetCommandPool");
	fn.allocate_command_buffers = device_proc<PFN_vkAllocateCommandBuffers>(dp, m_device, "vkAllocateCommandBuffers");
	fn.free_command_buffers = device_proc<PFN_vkFreeCommandBuffers>(dp, m_device, "vkFreeCommandBuffers");
	fn.begin_command_buffer = device_proc<PFN_vkBeginCommandBuffer>(dp, m_device, "vkBeginCommandBuffer");
	fn.end_command_buffer = device_proc<PFN_vkEndCommandBuffer>(dp, m_device, "vkEndCommandBuffer");
	fn.queue_submit = device_proc<PFN_vkQueueSubmit>(dp, m_device, "vkQueueSubmit");
	fn.queue_wait_idle = device_proc<PFN_vkQueueWaitIdle>(dp, m_device, "vkQueueWaitIdle");
	fn.create_fence = device_proc<PFN_vkCreateFence>(dp, m_device, "vkCreateFence");
	fn.destroy_fence = device_proc<PFN_vkDestroyFence>(dp, m_device, "vkDestroyFence");
	fn.wait_for_fences = device_proc<PFN_vkWaitForFences>(dp, m_device, "vkWaitForFences");
	fn.reset_fences = device_proc<PFN_vkResetFences>(dp, m_device, "vkResetFences");
	fn.get_fence_status = device_proc<PFN_vkGetFenceStatus>(dp, m_device, "vkGetFenceStatus");
	fn.allocate_memory = device_proc<PFN_vkAllocateMemory>(dp, m_device, "vkAllocateMemory");
	fn.free_memory = device_proc<PFN_vkFreeMemory>(dp, m_device, "vkFreeMemory");
	fn.map_memory = device_proc<PFN_vkMapMemory>(dp, m_device, "vkMapMemory");
	fn.unmap_memory = device_proc<PFN_vkUnmapMemory>(dp, m_device, "vkUnmapMemory");
	fn.flush_mapped_memory_ranges = device_proc<PFN_vkFlushMappedMemoryRanges>(dp, m_device, "vkFlushMappedMemoryRanges");
	fn.invalidate_mapped_memory_ranges = device_proc<PFN_vkInvalidateMappedMemoryRanges>(dp, m_device, "vkInvalidateMappedMemoryRanges");
	fn.create_buffer = device_proc<PFN_vkCreateBuffer>(dp, m_device, "vkCreateBuffer");
	fn.destroy_buffer = device_proc<PFN_vkDestroyBuffer>(dp, m_device, "vkDestroyBuffer");
	fn.get_buffer_memory_requirements = device_proc<PFN_vkGetBufferMemoryRequirements>(dp, m_device, "vkGetBufferMemoryRequirements");
	fn.bind_buffer_memory = device_proc<PFN_vkBindBufferMemory>(dp, m_device, "vkBindBufferMemory");

	fn.get_device_queue(m_device, m_queue_family, 0, &m_queue);

	VkCommandPoolCreateInfo pool_info = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
	pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pool_info.queueFamilyIndex = m_queue_family;
	if (fn.create_command_pool(m_device, &pool_info, nullptr, &m_pool) != VK_SUCCESS)
	{
		report = "vkCreateCommandPool failed";
		destroy();
		return false;
	}

	report = std::string("device \"") + m_props.deviceName + "\" (" + type_name(m_props.deviceType) + ", Vulkan " +
			api_string(m_props.apiVersion) + ", dualSrcBlend " + (m_dual_src_blend ? "yes" : "no") + ")";
	return true;
}

void vk_device::destroy()
{
	if (m_device != VK_NULL_HANDLE)
	{
		fn.device_wait_idle(m_device);
		if (m_pool != VK_NULL_HANDLE)
			fn.destroy_command_pool(m_device, m_pool, nullptr);
		fn.destroy_device(m_device, nullptr);
	}
	if (m_instance != VK_NULL_HANDLE)
		fn.destroy_instance(m_instance, nullptr);
	m_pool = VK_NULL_HANDLE;
	m_device = VK_NULL_HANDLE;
	m_queue = VK_NULL_HANDLE;
	m_instance = VK_NULL_HANDLE;
	m_physical = VK_NULL_HANDLE;
}

void vk_device::latch(const char *what, const char *detail)
{
	if (m_failed)
		return;
	m_failed = true;
	fprintf(stderr, "[retro_vk] %s: %s; Vulkan engine off for this session\n", what, detail);
}

uint32_t vk_device::find_memory_type(uint32_t type_bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags excluded) const
{
	for (uint32_t i = 0; i < m_memory.memoryTypeCount; i++)
	{
		const VkMemoryPropertyFlags flags = m_memory.memoryTypes[i].propertyFlags;
		if ((type_bits & (1U << i)) && ((flags & want) == want) && !(flags & excluded))
			return i;
	}
	return UINT32_MAX;
}

VkDeviceMemory vk_device::allocate(const VkMemoryRequirements &req, VkMemoryPropertyFlags want, VkMemoryPropertyFlags excluded)
{
	const uint32_t type = find_memory_type(req.memoryTypeBits, want, excluded);
	if (type == UINT32_MAX)
		return VK_NULL_HANDLE;
	VkMemoryAllocateInfo info = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	info.allocationSize = req.size;
	info.memoryTypeIndex = type;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	if (fn.allocate_memory(m_device, &info, nullptr, &memory) != VK_SUCCESS)
		return VK_NULL_HANDLE;
	return memory;
}

} // namespace retro_vk
