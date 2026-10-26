// license:BSD-3-Clause
// copyright-holders:TrickoUK
/***************************************************************************

    retro_vk_loader.cpp

    Headless Vulkan device probe for the libretro OSD (see retro_vk_loader.h).

***************************************************************************/

#include "retro_vk_loader.h"

#define VK_NO_PROTOTYPES
#include "../../../../3rdparty/bgfx/3rdparty/khronos/vulkan-local/vulkan_core.h"

#include <dlfcn.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace retro_vk {

namespace {

// Entry points the probe needs. Everything else is reached through the
// engine's own device once it exists.
struct vk_api
{
	PFN_vkGetInstanceProcAddr get_instance_proc_addr = nullptr;
	PFN_vkEnumerateInstanceVersion enumerate_instance_version = nullptr;
	PFN_vkCreateInstance create_instance = nullptr;
	PFN_vkDestroyInstance destroy_instance = nullptr;
	PFN_vkEnumeratePhysicalDevices enumerate_physical_devices = nullptr;
	PFN_vkGetPhysicalDeviceProperties2 get_physical_device_properties2 = nullptr;
	PFN_vkGetPhysicalDeviceFeatures2 get_physical_device_features2 = nullptr;
	PFN_vkGetPhysicalDeviceQueueFamilyProperties get_queue_family_properties = nullptr;
};

// Device types in the order the probe prefers them. The CPU (software)
// implementation is never chosen automatically.
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

bool has_graphics_queue(const vk_api &api, VkPhysicalDevice device)
{
	uint32_t count = 0;
	api.get_queue_family_properties(device, &count, nullptr);
	std::vector<VkQueueFamilyProperties> families(count);
	api.get_queue_family_properties(device, &count, families.data());
	for (const auto &family : families)
		if (family.queueFlags & VK_QUEUE_GRAPHICS_BIT)
			return true;
	return false;
}

} // anonymous namespace

vk_probe_result retro_vk_probe()
{
	vk_probe_result result;

	// The library is deliberately never closed: some ICDs misbehave when unloaded, and the
	// Vulkan engine reuses the same handle.
	void *const lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
	if (!lib)
	{
		result.report = "libvulkan.so.1 not found";
		return result;
	}

	vk_api api;
	api.get_instance_proc_addr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib, "vkGetInstanceProcAddr"));
	if (!api.get_instance_proc_addr)
	{
		result.report = "libvulkan.so.1 has no vkGetInstanceProcAddr";
		return result;
	}
	api.enumerate_instance_version = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
			api.get_instance_proc_addr(nullptr, "vkEnumerateInstanceVersion"));
	api.create_instance = reinterpret_cast<PFN_vkCreateInstance>(api.get_instance_proc_addr(nullptr, "vkCreateInstance"));
	if (!api.enumerate_instance_version || !api.create_instance)
	{
		// a loader without vkEnumerateInstanceVersion is Vulkan 1.0 only
		result.report = "Vulkan loader is older than 1.1";
		return result;
	}

	uint32_t loader_version = 0;
	api.enumerate_instance_version(&loader_version);
	if (loader_version < vk_required_api_version)
	{
		result.report = "Vulkan loader is " + api_string(loader_version) + ", need 1.3";
		return result;
	}

	VkApplicationInfo app_info = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
	app_info.pApplicationName = "MAME (libretro)";
	app_info.apiVersion = vk_required_api_version;
	VkInstanceCreateInfo instance_info = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
	instance_info.pApplicationInfo = &app_info;

	// headless: no surface extensions, no layers (MAME_GPU_VK_VALIDATE is for the engine)
	VkInstance instance = VK_NULL_HANDLE;
	if (api.create_instance(&instance_info, nullptr, &instance) != VK_SUCCESS)
	{
		result.report = "vkCreateInstance failed";
		return result;
	}

	api.destroy_instance = reinterpret_cast<PFN_vkDestroyInstance>(api.get_instance_proc_addr(instance, "vkDestroyInstance"));
	api.enumerate_physical_devices = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
			api.get_instance_proc_addr(instance, "vkEnumeratePhysicalDevices"));
	api.get_physical_device_properties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
			api.get_instance_proc_addr(instance, "vkGetPhysicalDeviceProperties2"));
	api.get_physical_device_features2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
			api.get_instance_proc_addr(instance, "vkGetPhysicalDeviceFeatures2"));
	api.get_queue_family_properties = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
			api.get_instance_proc_addr(instance, "vkGetPhysicalDeviceQueueFamilyProperties"));

	uint32_t device_count = 0;
	api.enumerate_physical_devices(instance, &device_count, nullptr);
	std::vector<VkPhysicalDevice> devices(device_count);
	if (device_count)
		api.enumerate_physical_devices(instance, &device_count, devices.data());

	// MAME_GPU_VK_DEVICE: an index into the list, or a substring of the device name
	const char *const wanted = std::getenv("MAME_GPU_VK_DEVICE");

	VkPhysicalDevice best = VK_NULL_HANDLE;
	int best_rank = 1000;
	VkPhysicalDeviceProperties2 best_props = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
	VkPhysicalDeviceFeatures2 best_features = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };

	for (uint32_t i = 0; i < device_count; i++)
	{
		VkPhysicalDeviceProperties2 props = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
		api.get_physical_device_properties2(devices[i], &props);
		VkPhysicalDeviceFeatures2 features = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
		api.get_physical_device_features2(devices[i], &features);

		bool const selected_by_name = wanted && (strcmp(wanted, std::to_string(i).c_str()) == 0 || strstr(props.properties.deviceName, wanted));
		if (wanted && !selected_by_name)
			continue;
		if (props.properties.apiVersion < vk_required_api_version || !has_graphics_queue(api, devices[i]))
			continue;

		// the CPU implementation is only used when named explicitly (useful for lavapipe)
		int const rank = (props.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU)
				? (wanted ? 0 : -1)
				: type_rank(props.properties.deviceType);
		if (rank < 0)
			continue;
		if (!best || rank < best_rank)
		{
			best = devices[i];
			best_rank = rank;
			best_props = props;
			best_features = features;
		}
	}

	if (best)
	{
		result.available = true;
		result.dual_src_blend = best_features.features.dualSrcBlend;
		result.report = std::string("device \"") + best_props.properties.deviceName + "\" (" +
				type_name(best_props.properties.deviceType) + ", Vulkan " + api_string(best_props.properties.apiVersion) +
				", dualSrcBlend " + (result.dual_src_blend ? "yes" : "no") + ")";
	}
	else if (wanted)
		result.report = std::string("no usable device matches MAME_GPU_VK_DEVICE=") + wanted;
	else
		result.report = "no device with Vulkan 1.3 and a graphics queue";

	api.destroy_instance(instance, nullptr);
	return result;
}

} // namespace retro_vk
