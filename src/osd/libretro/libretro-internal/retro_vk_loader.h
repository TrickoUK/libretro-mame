// license:BSD-3-Clause
// copyright-holders:TrickoUK
/***************************************************************************

    retro_vk_loader.h

    Runtime-loaded Vulkan entry points for the libretro OSD's host-GPU
    backends. The loader is dlopen()ed like retro_gl_loader: libvulkan is
    not a build-time dependency, so the feature degrades to "unavailable"
    on any system without a working Vulkan loader and ICD.

    The Vulkan types come from the Khronos headers vendored with bgfx;
    VK_NO_PROTOTYPES keeps the header from declaring the entry points, so
    every call goes through the function pointers in vk_api.

    retro_vk_probe() creates a headless instance, looks for a device that
    meets the engine's requirements (Vulkan 1.3, see
    retro_vk_device_requirements) and reports which one it would use. It
    shares no state with RetroArch's own Vulkan instance.

***************************************************************************/
#ifndef MAME_OSD_LIBRETRO_RETRO_VK_LOADER_H
#define MAME_OSD_LIBRETRO_RETRO_VK_LOADER_H

#pragma once

#include <string>

namespace retro_vk {

// Result of retro_vk_probe(). The probe itself never fails the core: a
// missing loader or device sets available = false and says why in report.
struct vk_probe_result
{
	bool available = false;     // a device meeting the requirements was found
	bool dual_src_blend = false; // that device supports dualSrcBlend (System 22 needs it)
	std::string report;          // one line for the log: chosen device, or why none was usable
};

// Requirements an engine device must meet (decision 8.4 of gpu-vulkan-plan.md).
constexpr unsigned int vk_required_api_version = 0x00403000; // VK_MAKE_API_VERSION(0, 1, 3, 0)

// Probes the system once and returns the result. Honours MAME_GPU_VK_DEVICE
// (index into the physical device list, or a name substring) when set.
vk_probe_result retro_vk_probe();

} // namespace retro_vk

#endif // MAME_OSD_LIBRETRO_RETRO_VK_LOADER_H
