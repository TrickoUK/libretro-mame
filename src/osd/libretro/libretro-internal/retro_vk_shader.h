// license:BSD-3-Clause
// copyright-holders:TrickoUK
/***************************************************************************

    retro_vk_shader.h

    Vulkan side of the host-GPU engine's shaders. A module's GLSL 330
    fragment source is wrapped in the same kind of prelude the OpenGL engine
    uses (retro_gpu_engine.cpp get_program), but written for GLSL 450 with
    Vulkan's binding model: the per-frame values are push constants, the
    per-primitive state is a storage buffer of int, and the samplers are in
    descriptor set 0. The module source itself is unchanged.

    Compilation is GLSL to SPIR-V at run time through glslang
    (gpu-vulkan-plan.md decision 8.1). Compiled SPIR-V is kept on disk under
    <system>/mame/gpu-cache, keyed by a hash of the stage, the generated
    source and a caller-supplied key (the driver version once the Vulkan
    engine knows it).

***************************************************************************/
#ifndef MAME_OSD_LIBRETRO_RETRO_VK_SHADER_H
#define MAME_OSD_LIBRETRO_RETRO_VK_SHADER_H

#pragma once

#include "interface/gpuengine.h"

#include <cstdint>
#include <string>
#include <vector>

namespace retro_vk {

enum class shader_stage { VERTEX, FRAGMENT, COMPUTE };

// Vulkan GLSL 450 source for the vertex stage of a module. It depends only on
// the module's varyings, so it is the same for every variant.
std::string vertex_glsl(const osd::gpu_shader_module &module);

// Vulkan GLSL 450 source for the fragment stage of one module and variant
// (variant bits as in gpu_aniso_variant()).
std::string fragment_glsl(const osd::gpu_shader_module &module, uint32_t variant);

// Compiles GLSL 450 to SPIR-V 1.6 for Vulkan 1.3. Returns false with the
// compiler log in log when the source does not compile.
bool compile_glsl(shader_stage stage, const std::string &source, std::vector<uint32_t> &spirv, std::string &log);

// As compile_glsl(), with the SPIR-V kept in cache_dir: a cached file is used
// when present and written after a successful compile. An empty cache_dir
// disables the cache.
bool compile_cached(shader_stage stage, const std::string &source, const std::string &cache_key,
		const std::string &cache_dir, std::vector<uint32_t> &spirv, std::string &log);

// The cache key for compile_cached(): the compiler and prelude versions followed by the
// caller's device key, which names the vendor, device and driver version.
std::string shader_cache_key(const std::string &device_key);

// Compiles both stages of one module and variant and logs the outcome.
// Used by MAME_GPU_VK_SHADERCHECK to check every program the GL engine
// builds. Returns true when both stages compile.
bool check_module(const osd::gpu_shader_module &module, uint32_t variant, const std::string &cache_dir);

} // namespace retro_vk

#endif // MAME_OSD_LIBRETRO_RETRO_VK_SHADER_H
