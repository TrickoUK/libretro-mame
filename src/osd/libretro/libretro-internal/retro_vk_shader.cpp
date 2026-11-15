// license:BSD-3-Clause
// copyright-holders:TrickoUK
/***************************************************************************

    retro_vk_shader.cpp

    GLSL 450 preludes for the Vulkan engine and their glslang compilation
    (see retro_vk_shader.h).

***************************************************************************/

#include "retro_vk_shader.h"

#include "glslang/Public/ResourceLimits.h"
#include "glslang/Public/ShaderLang.h"
#include "glslang/SPIRV/GlslangToSpv.h"
#include "glslang/build_info.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace retro_vk {

namespace {

// First word of every SPIR-V module
constexpr uint32_t SPIRV_MAGIC = 0x07230203;

// Bump when vertex_glsl() or fragment_glsl() change what they generate, so
// cached SPIR-V from the old prelude is not reused.
constexpr int PRELUDE_VERSION = 1;

} // anonymous namespace

namespace {

const char *sampler_type(osd::gpu_texture_format f)
{
	switch (f)
	{
	case osd::gpu_texture_format::RGBA32I:  return "isampler2D";
	case osd::gpu_texture_format::RGBA8:    return "sampler2D";
	default:                                return "usampler2D";
	}
}

std::string interpolation_qualifier(osd::gpu_varying_mode m, bool centroid)
{
	std::string q;
	switch (m)
	{
	case osd::gpu_varying_mode::NOPERSPECTIVE:  q = "noperspective "; break;
	case osd::gpu_varying_mode::FLAT:           return "flat ";
	case osd::gpu_varying_mode::PERSPECTIVE:    break;
	}
	if (centroid)
		q += "centroid ";
	return q;
}

// Per-frame values, shared by both stages. Offsets are std430 push-constant
// offsets: vec2 at 0, float at 8, int at 12 and 16.
constexpr const char *PUSH_CONSTANTS =
		"layout(push_constant) uniform GpuPush\n{\n"
		"\tvec2 target_size;\n"
		"\tfloat order_scale;\n"
		"\tint depth_order;\n"
		"\tint rec_texels;\n"
		"} gpu_push;\n";

// FNV-1a, 64 bit: a stable name for a cache entry
uint64_t fnv1a(const std::string &s, uint64_t h = 0xcbf29ce484222325ULL)
{
	for (unsigned char c : s)
	{
		h ^= c;
		h *= 0x100000001b3ULL;
	}
	return h;
}

// Reads a cache entry. Anything that is not a whole SPIR-V module (a
// truncated write, a file from another tool) is rejected, so it is compiled
// again and overwritten.
bool read_file(const std::string &path, std::vector<uint32_t> &words)
{
	std::ifstream in(path, std::ios::binary);
	if (!in)
		return false;
	std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	if (bytes.size() < 4 || (bytes.size() % 4) != 0)
		return false;
	words.resize(bytes.size() / 4);
	std::memcpy(words.data(), bytes.data(), bytes.size());
	return words[0] == SPIRV_MAGIC;
}

// Writes to a temporary name and renames it into place, so a run killed
// mid-write leaves no entry at the final name.
bool write_file(const std::string &path, const std::vector<uint32_t> &words)
{
	const std::string temp = path + ".tmp";
	{
		std::ofstream out(temp, std::ios::binary | std::ios::trunc);
		if (!out)
			return false;
		out.write(reinterpret_cast<const char *>(words.data()), std::streamsize(words.size() * 4));
		out.close();
		if (!out)
		{
			std::remove(temp.c_str());
			return false;
		}
	}
	if (std::rename(temp.c_str(), path.c_str()) != 0)
	{
		std::remove(temp.c_str());
		return false;
	}
	return true;
}

// Names the compiler and the prelude, so an update of either invalidates
// the cache. The Vulkan engine adds the driver version to the key.
std::string compiler_cache_key()
{
	return std::string("glslang-") + std::to_string(GLSLANG_VERSION_MAJOR) + "." + std::to_string(GLSLANG_VERSION_MINOR) +
			"." + std::to_string(GLSLANG_VERSION_PATCH) + "-prelude" + std::to_string(PRELUDE_VERSION);
}

// glslang is initialised once for the process and never finalised: the
// engine keeps compiling for as long as the core runs.
bool glslang_ready()
{
	static const bool ready = glslang::InitializeProcess();
	return ready;
}

} // anonymous namespace

std::string vertex_glsl(const osd::gpu_shader_module &module)
{
	const int groups = (module.varying_count + 3) / 4;
	std::string src = "#version 450\n";
	src += "layout(location = 0) in vec4 a_pos;\n";
	src += "layout(location = 1) in uint a_prim;\n";
	for (int g = 0; g < groups; g++)
		src += "layout(location = " + std::to_string(2 + g) + ") in vec4 a_var" + std::to_string(g) + ";\n";
	src += PUSH_CONSTANTS;
	src += "layout(location = 0) flat out uint g_prim;\n";
	for (int i = 0; i < module.varying_count; i++)
		src += "layout(location = " + std::to_string(1 + i) + ") " +
				interpolation_qualifier(module.varying_mode[i], ((module.centroid_mask >> i) & 1) != 0) +
				"out float v" + std::to_string(i) + ";\n";
	src += "void main()\n{\n";
	// Vulkan's framebuffer origin is the top-left, so target y = 0 maps to NDC -1 and rows
	// are read back in target order without a flip.
	src += "\tvec2 ndc = a_pos.xy / gpu_push.target_size * 2.0 - 1.0;\n";
	src += "\tfloat z = (gpu_push.depth_order != 0) ? (float(a_prim) + 1.0) * gpu_push.order_scale : a_pos.z;\n";
	src += "\tfloat w = a_pos.w;\n";
	// Vulkan clip depth is [0,1], so the depth goes in unchanged.
	src += "\tgl_Position = vec4(ndc * w, z * w, w);\n";
	src += "\tg_prim = a_prim;\n";
	static const char *const swizzle = "xyzw";
	for (int i = 0; i < module.varying_count; i++)
		src += "\tv" + std::to_string(i) + " = a_var" + std::to_string(i / 4) + "." + swizzle[i % 4] + ";\n";
	src += "}\n";
	return src;
}

std::string fragment_glsl(const osd::gpu_shader_module &module, uint32_t variant)
{
	// Bits 24-27 of the variant carry log2 of the anisotropic tap limit (0 = off); the module sees
	// the low 24 bits as VARIANT and the limit as ANISO_MAX.
	const uint32_t aniso_log2 = (variant >> 24) & 15;
	std::string src = "#version 450\n";
	src += "#define VARIANT " + std::to_string(variant & 0xffffffU) + "\n";
	src += "#define ANISO_MAX " + std::to_string(aniso_log2 ? (1U << aniso_log2) : 0U) + "\n";
	src += "layout(location = 0) flat in uint g_prim;\n";
	for (int i = 0; i < module.varying_count; i++)
		src += "layout(location = " + std::to_string(1 + i) + ") " +
				interpolation_qualifier(module.varying_mode[i], ((module.centroid_mask >> i) & 1) != 0) +
				"in float v" + std::to_string(i) + ";\n";
	if (module.blend_factor_output)
		src += "layout(location = 0, index = 0) out vec4 o_color;\nlayout(location = 0, index = 1) out vec4 o_blend;\nfloat blend_factor = 1.0;\n";
	else
		src += "layout(location = 0) out vec4 o_color;\n";
	src += PUSH_CONSTANTS;
	// per-primitive state: rec_texels texels of four ints per record, read as plain ints
	src += "layout(std430, set = 0, binding = 0) readonly buffer State\n{\n\tint words[];\n} state_buf;\n";
	src += "int state(int k)\n{\n";
	src += "\treturn state_buf.words[int(g_prim) * gpu_push.rec_texels * 4 + k];\n}\n";
	src += "vec2 frag_pos()\n{\n\treturn gl_FragCoord.xy;\n}\n";
	src += "ivec2 target_size()\n{\n\treturn ivec2(gpu_push.target_size);\n}\n";
	// Anisotropic sampling helpers for the module's samplers.
	src += "int aniso_taps(vec2 dx, vec2 dy, int limit, out vec2 axis)\n{\n"
			"\tfloat lx = length(dx);\n\tfloat ly = length(dy);\n"
			"\taxis = (lx >= ly) ? dx : dy;\n"
			"\tfloat major = max(lx, ly);\n"
			"\tif (limit < 2 || !(major > 1.0) || !(major < 1e9))\n\t\treturn 1;\n"
			"\tfloat minor = max(abs(dx.x * dy.y - dx.y * dy.x) / major, 1e-6);\n"
			"\tfloat ratio = major / minor;\n"
			"\tif (ratio < 1.5)\n\t\treturn 1;\n"
			"\treturn clamp(int(ceil(ratio)), 1, min(limit, int(ceil(major))));\n}\n";
	src += "void aniso_grid(vec2 dx, vec2 dy, int limit, out int nx, out int ny)\n{\n"
			"\tnx = 1;\n\tny = 1;\n"
			"\tfloat lx = length(dx);\n\tfloat ly = length(dy);\n"
			"\tif (limit < 2 || !(lx < 1e9) || !(ly < 1e9))\n\t\treturn;\n"
			"\tfloat wx = max(floor(lx + 0.5), 1.0);\n\tfloat wy = max(floor(ly + 0.5), 1.0);\n"
			"\tfloat fl = float(limit);\n"
			"\tif (wx * wy > fl) {\n"
			"\t\tfloat f = sqrt(fl / (wx * wy));\n"
			"\t\tfloat fx = max(floor(wx * f), 1.0);\n\t\tfloat fy = max(floor(wy * f), 1.0);\n"
			"\t\tif (fx <= 1.0)\n\t\t\tfy = min(wy, fl);\n"
			"\t\telse if (fy <= 1.0)\n\t\t\tfx = min(wx, fl);\n"
			"\t\twx = fx;\n\t\twy = fy;\n\t}\n"
			"\tnx = int(wx);\n\tny = int(wy);\n}\n";
	for (int i = 0; i < module.binding_count; i++)
	{
		// samplers sit in set 0 after the state buffer at binding 0
		src += "layout(set = 0, binding = " + std::to_string(1 + i) + ") uniform " +
				sampler_type(module.binding_format[i]) + " " + module.binding_name[i] + ";\n";
		src += "ivec2 " + std::string(module.binding_name[i]) + "_size()\n{\n\treturn textureSize(" +
				module.binding_name[i] + ", 0);\n}\n";
	}
	if (module.writes_depth)
		src += "float frag_depth = 0.0;\n";
	src += module.fragment_source;
	src += "\n";
	src += "void main()\n{\n\tvec4 c;\n\tif (!shade(c))\n\t\tdiscard;\n\to_color = c;\n";
	if (module.blend_factor_output)
		src += "\to_blend = vec4(blend_factor);\n";
	if (module.writes_depth)
		src += "\tgl_FragDepth = frag_depth;\n";
	src += "}\n";
	return src;
}

bool compile_glsl(shader_stage stage, const std::string &source, std::vector<uint32_t> &spirv, std::string &log)
{
	if (!glslang_ready())
	{
		log = "glslang failed to initialise";
		return false;
	}

	const EShLanguage language = (stage == shader_stage::VERTEX) ? EShLangVertex
			: (stage == shader_stage::FRAGMENT) ? EShLangFragment : EShLangCompute;
	glslang::TShader shader(language);
	const char *strings[1] = { source.c_str() };
	shader.setStrings(strings, 1);
	shader.setEnvInput(glslang::EShSourceGlsl, language, glslang::EShClientVulkan, 450);
	shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_3);
	shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_6);

	const EShMessages messages = static_cast<EShMessages>(EShMsgDefault | EShMsgSpvRules | EShMsgVulkanRules);
	if (!shader.parse(GetDefaultResources(), 450, false, messages))
	{
		log = shader.getInfoLog();
		return false;
	}

	glslang::TProgram program;
	program.addShader(&shader);
	if (!program.link(messages))
	{
		log = program.getInfoLog();
		return false;
	}

	spirv.clear();
	glslang::GlslangToSpv(*program.getIntermediate(language), spirv);
	return true;
}

bool compile_cached(shader_stage stage, const std::string &source, const std::string &cache_key,
		const std::string &cache_dir, std::vector<uint32_t> &spirv, std::string &log)
{
	// the entry name covers the stage, the key and the generated source
	const uint64_t hash = fnv1a(source, fnv1a(cache_key, fnv1a(stage == shader_stage::VERTEX ? "v" : (stage == shader_stage::FRAGMENT) ? "f" : "c")));
	char name[32];
	std::snprintf(name, sizeof(name), "%016llx.spv", static_cast<unsigned long long>(hash));
	const std::string path = cache_dir.empty() ? std::string() : cache_dir + "/" + name;

	if (!path.empty() && read_file(path, spirv))
		return true;
	if (!compile_glsl(stage, source, spirv, log))
		return false;
	if (!path.empty())
	{
		// the directory is created on first use; a failed write only costs a recompile next time
		mkdir(cache_dir.c_str(), 0755);
		write_file(path, spirv);
	}
	return true;
}

// The key for compile_cached(): the compiler and prelude versions, then the device key the
// caller gives (vendor, device and driver version), so a driver update invalidates the cache.
std::string shader_cache_key(const std::string &device_key)
{
	return compiler_cache_key() + "-" + device_key;
}

} // namespace retro_vk
