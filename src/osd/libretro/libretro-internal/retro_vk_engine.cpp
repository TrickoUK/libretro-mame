// license:BSD-3-Clause
// copyright-holders:TrickoUK
/***************************************************************************
    retro_vk_engine.cpp
    Vulkan implementation of osd::gpu_engine (see retro_vk_engine.h). Every
    image is kept in VK_IMAGE_LAYOUT_GENERAL, so transfers and clears need
    no layout changes. The colour target is B8G8R8A8, so a read-back copies
    the bytes the OpenGL engine's GL_BGRA read-back produces, and the RG8
    read-back takes the red and green bytes as the OpenGL one does.
***************************************************************************/

#include "retro_vk_engine.h"
#include "libretro_shared.h"
#include "retro_vk_shader.h"

#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>


namespace {

using clock_type = std::chrono::steady_clock;

double elapsed_ms(clock_type::time_point from)
{
	return std::chrono::duration<double, std::milli>(clock_type::now() - from).count();
}

struct format_info
{
	VkFormat vk;
	int bytes;
};

format_info get_format_info(osd::gpu_texture_format f)
{
	switch (f)
	{
	case osd::gpu_texture_format::R8UI:     return { VK_FORMAT_R8_UINT, 1 };
	case osd::gpu_texture_format::R16UI:    return { VK_FORMAT_R16_UINT, 2 };
	case osd::gpu_texture_format::R32UI:    return { VK_FORMAT_R32_UINT, 4 };
	case osd::gpu_texture_format::RG32UI:   return { VK_FORMAT_R32G32_UINT, 8 };
	case osd::gpu_texture_format::RGBA32I:  return { VK_FORMAT_R32G32B32A32_SINT, 16 };
	case osd::gpu_texture_format::RGBA8:    break;
	}
	return { VK_FORMAT_R8G8B8A8_UNORM, 4 };
}

// descriptor sets per pool; a frame that needs more gets another pool
constexpr uint32_t DESCRIPTOR_SETS_PER_POOL = 4096;

// the vertex attribute format of a varying group with 1 to 4 components
constexpr VkFormat VARYING_FORMATS[4] = {
		VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT };

// the file name of the pipeline cache under the system's gpu-cache directory
constexpr const char *PIPELINE_CACHE_NAME = "vk-pipelines.bin";

// The per-frame values, in the order the prelude declares them (std430 offsets 0, 8, 12 and 16).
struct push_constants
{
	float target_size[2];
	float order_scale;
	int32_t depth_order;
	int32_t rec_texels;
};
static_assert(sizeof(push_constants) == 20, "push constants match the shader prelude");

// The read-back pass: the rectangle's top-left and output size, the downsample factor, the flags (1 channel lookup,
// 2 opaque, 4 coverage, 8 coverage share, 16 priority bits), the word offsets of the output and of the lookup
// table in the staging buffer, and the samples read per pixel (1 unless the target is multisampled and read sample by sample).
struct output_constants
{
	int32_t origin[2];
	int32_t size[2];
	int32_t ds;
	int32_t flags;
	int32_t out_at;
	int32_t lut_at;
	int32_t samples;
};
static_assert(sizeof(output_constants) == 36, "output constants match the read-back shader");

// The read-back compute pass. It averages each ds-by-ds block of the target (through the same integer arithmetic
// as the OpenGL output pass, so both engines give the same bytes), maps the colour through the lookup table and
// writes the output pixels as BGRA bytes, one word per pixel. @SAMPLER@ and @SAMPLE@ are replaced by output_source():
// a sampler2DMS read sample by sample, or a sampler2D read once.
constexpr const char *OUTPUT_GLSL = R"(#version 450
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
layout(std430, set = 0, binding = 0) buffer Staging
{
	uint words[];
};
layout(set = 0, binding = 1) uniform @SAMPLER@ u_src;
layout(push_constant) uniform Pass
{
	ivec2 origin;
	ivec2 size;
	int ds;
	int flags;
	int out_at;
	int lut_at;
	int samples;
};
void main()
{
	const ivec2 o = ivec2(gl_GlobalInvocationID.xy);
	if (o.x >= size.x || o.y >= size.y)
		return;
	const ivec2 base = o * ds + origin;
	ivec4 sum = ivec4(0);
	int count = 0;
	int drawn = 0;
	int high = 0;
	for (int y = 0; y < ds; y++)
	{
		for (int x = 0; x < ds; x++)
		{
			for (int s = 0; s < samples; s++)
			{
				const ivec4 t = ivec4(texelFetch(u_src, base + ivec2(x, y), @SAMPLE@) * 255.0 + 0.5);
				if (t.a != 0)
				{
					drawn++;
					if (t.a == 255)
						high++;
				}
				if ((flags & 4) == 0 || t.a != 0)
				{
					sum += t;
					count++;
				}
			}
		}
	}
	ivec4 v = (count > 0) ? (sum / count) : ivec4(0);
	if ((flags & 1) != 0)
	{
		v.r = int(words[lut_at + (v.r >> 3)]);
		v.g = int(words[lut_at + 32 + (v.g >> 2)]);
		v.b = int(words[lut_at + 96 + (v.b >> 3)]);
	}
	if ((flags & 2) != 0)
		v.a = 255;
	if ((flags & 16) != 0)
		v.a = (drawn == 0) ? 0 : (high * 2 > drawn) ? 255 : 128;
	if ((flags & 4) != 0)
	{
		const int total = ds * ds * samples;
		v.a = (count == 0) ? 0 : ((flags & 8) != 0) ? max(1, (255 * count + total / 2) / total) : 255;
	}
	const uint packed = uint(v.b) | (uint(v.g) << 8) | (uint(v.r) << 16) | (uint(v.a) << 24);
	words[out_at + o.y * size.x + o.x] = packed;
}
)";

// The read-back shader for a single-sample target (or one read once) or for a multisample target read sample by sample.
std::string output_source(bool per_sample)
{
	std::string source = OUTPUT_GLSL;
	auto replace = [&source](const char *token, const char *with)
	{
		for (size_t at = source.find(token); at != std::string::npos; at = source.find(token, at + std::strlen(with)))
			source.replace(at, std::strlen(token), with);
	};
	replace("@SAMPLER@", per_sample ? "sampler2DMS" : "sampler2D");
	replace("@SAMPLE@", per_sample ? "s" : "0");
	return source;
}

VkBlendFactor blend_factor_vk(osd::gpu_blend_factor f)
{
	switch (f)
	{
	case osd::gpu_blend_factor::ZERO:                       return VK_BLEND_FACTOR_ZERO;
	case osd::gpu_blend_factor::ONE:                        return VK_BLEND_FACTOR_ONE;
	case osd::gpu_blend_factor::SRC_ALPHA:                  return VK_BLEND_FACTOR_SRC_ALPHA;
	case osd::gpu_blend_factor::ONE_MINUS_SRC_ALPHA:        return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	case osd::gpu_blend_factor::CONSTANT_ALPHA:             return VK_BLEND_FACTOR_CONSTANT_ALPHA;
	case osd::gpu_blend_factor::ONE_MINUS_CONSTANT_ALPHA:   return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
	case osd::gpu_blend_factor::SRC_COLOR:                  return VK_BLEND_FACTOR_SRC_COLOR;
	case osd::gpu_blend_factor::ONE_MINUS_SRC_COLOR:        return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
	case osd::gpu_blend_factor::DST_COLOR:                  return VK_BLEND_FACTOR_DST_COLOR;
	case osd::gpu_blend_factor::ONE_MINUS_DST_COLOR:        return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
	case osd::gpu_blend_factor::SRC1_ALPHA:                 return VK_BLEND_FACTOR_SRC1_ALPHA;
	case osd::gpu_blend_factor::ONE_MINUS_SRC1_ALPHA:       return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
	case osd::gpu_blend_factor::SRC_ALPHA_SATURATE:         break;
	}
	return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
}

VkBlendOp blend_op_vk(osd::gpu_blend_op op)
{
	switch (op)
	{
	case osd::gpu_blend_op::ADD:                return VK_BLEND_OP_ADD;
	case osd::gpu_blend_op::SUBTRACT:           return VK_BLEND_OP_SUBTRACT;
	case osd::gpu_blend_op::REVERSE_SUBTRACT:   return VK_BLEND_OP_REVERSE_SUBTRACT;
	}
	return VK_BLEND_OP_ADD;
}

VkCompareOp depth_func_vk(osd::gpu_depth_func f)
{
	switch (f)
	{
	case osd::gpu_depth_func::NEVER:    return VK_COMPARE_OP_NEVER;
	case osd::gpu_depth_func::LESS:     return VK_COMPARE_OP_LESS;
	case osd::gpu_depth_func::LEQUAL:   return VK_COMPARE_OP_LESS_OR_EQUAL;
	case osd::gpu_depth_func::EQUAL:    return VK_COMPARE_OP_EQUAL;
	case osd::gpu_depth_func::GEQUAL:   return VK_COMPARE_OP_GREATER_OR_EQUAL;
	case osd::gpu_depth_func::GREATER:  return VK_COMPARE_OP_GREATER;
	case osd::gpu_depth_func::NOTEQUAL: return VK_COMPARE_OP_NOT_EQUAL;
	case osd::gpu_depth_func::ALWAYS:   break;
	}
	return VK_COMPARE_OP_ALWAYS;
}

// A cache file is loaded only when its header names this device and driver (VkPipelineCacheHeaderVersionOne: header
// size, header version, vendor ID, device ID, pipeline cache UUID). Anything else is ignored.
bool pipeline_cache_usable(const std::vector<uint8_t> &data, const VkPhysicalDeviceProperties &props)
{
	if (data.size() < 32)
		return false;
	uint32_t header[4] = {};
	std::memcpy(header, data.data(), sizeof(header));
	return header[0] == 32
			&& header[1] == VK_PIPELINE_CACHE_HEADER_VERSION_ONE
			&& header[2] == props.vendorID
			&& header[3] == props.deviceID
			&& std::memcmp(data.data() + 16, props.pipelineCacheUUID, VK_UUID_SIZE) == 0;
}

// the whole of a single-mip, single-layer image
VkImageSubresourceRange full_range(VkImageAspectFlags aspect)
{
	VkImageSubresourceRange range = {};
	range.aspectMask = aspect;
	range.levelCount = 1;
	range.layerCount = 1;
	return range;
}

VkImageSubresourceLayers layers(VkImageAspectFlags aspect)
{
	VkImageSubresourceLayers sub = {};
	sub.aspectMask = aspect;
	sub.layerCount = 1;
	return sub;
}

// staging offsets are kept on 16-byte boundaries, which satisfies every texel block size
// the smallest multiple of alignment that is not below value
size_t align_up(size_t value, size_t alignment)
{
	return (value + alignment - 1) / alignment * alignment;
}

} // anonymous namespace


retro_vk_engine::retro_vk_engine(retro_vk::vk_device &device)
	: m_device(device)
{
	if (!m_device.valid())
	{
		m_failed = true;
		return;
	}

	m_renderer = m_device.properties().deviceName;
	m_caps.renderer = m_renderer.c_str();
	m_caps.max_texture_size = int(std::min<uint32_t>(m_device.properties().limits.maxImageDimension2D, 16384));
	// submits are recorded and queued without waiting for the GPU, so a frame split into chunks overlaps emulation
	m_caps.chunked_frames = true;
	// the sample counts for colour and depth targets, and for sampled colour (read one sample at a time)
	const auto &limits = m_device.properties().limits;
	m_sample_counts = limits.framebufferColorSampleCounts & limits.framebufferDepthSampleCounts;
	m_sampled_sample_counts = limits.sampledImageColorSampleCounts;
	m_caps.max_samples = 0;
	for (int n = 2; n <= 64; n *= 2)
	{
		if (sample_count_supported(n, false))
			m_caps.max_samples = n;
	}
	if (m_device.sample_locations())
		fprintf(stderr, "[retro_vk_engine] VK_EXT_sample_locations: %s\n",
				m_device.variable_sample_locations() ? "variable locations, multisample-off draws put every sample at the pixel centre" : "not variable, snapping instead");

	const VkDevice dev = m_device.device();
	VkCommandBufferAllocateInfo alloc = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
	alloc.commandPool = m_device.command_pool();
	alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	alloc.commandBufferCount = 1;
	VkFenceCreateInfo fence_info = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	if (m_device.fn.allocate_command_buffers(dev, &alloc, &m_cmd) != VK_SUCCESS
			|| m_device.fn.create_fence(dev, &fence_info, nullptr, &m_fence) != VK_SUCCESS)
	{
		fprintf(stderr, "[retro_vk_engine] command buffer or fence creation failed\n");
		m_failed = true;
	}
	if (!m_failed && !create_pipeline_objects())
	{
		fprintf(stderr, "[retro_vk_engine] sampler, layout or pipeline cache creation failed\n");
		m_failed = true;
	}

	// timestamps need a queue that supports them; the valid bits of the queue family limit the counter's range
	m_timers = (std::getenv("MAME_GPU_PROF") != nullptr) || (std::getenv("MAME_VOODOO_GPU_PROF") != nullptr);
	if (m_timers && m_device.valid())
	{
		uint32_t count = 0;
		m_device.fn.get_queue_family_properties(m_device.physical_device(), &count, nullptr);
		std::vector<VkQueueFamilyProperties> families(count);
		if (m_device.queue_family() < count)
			m_device.fn.get_queue_family_properties(m_device.physical_device(), &count, families.data());
		const uint32_t bits = m_device.queue_family() < count ? families[m_device.queue_family()].timestampValidBits : 0;
		const VkPhysicalDeviceLimits &limits = m_device.properties().limits;
		m_timers = limits.timestampComputeAndGraphics && bits > 0;
		if (m_timers)
		{
			m_timestamp_mask = bits >= 64 ? ~uint64_t(0) : (uint64_t(1) << bits) - 1;
			m_timestamp_period = limits.timestampPeriod;
			VkQueryPoolCreateInfo query_info = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
			query_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
			query_info.queryCount = 2;
			m_timers = m_device.fn.create_query_pool(m_device.device(), &query_info, nullptr, &m_query_pool) == VK_SUCCESS;
		}
	}
}

retro_vk_engine::~retro_vk_engine()
{
	if (!m_device.valid())
		return;
	// nothing may be in flight while its objects are destroyed
	wait_in_flight();
	drop_pending();
	if (m_query_pool != VK_NULL_HANDLE)
		m_device.fn.destroy_query_pool(m_device.device(), m_query_pool, nullptr);
	for (auto &t : m_textures)
		destroy_image(t.second.img);
	m_textures.clear();
	release_target();
	destroy_pipeline_objects();
	if (m_fence != VK_NULL_HANDLE)
		m_device.fn.destroy_fence(m_device.device(), m_fence, nullptr);
	// the device itself is destroyed when m_device goes out of scope
}

void retro_vk_engine::log_problem(const char *what, const char *detail)
{
	m_last_problem = std::string(what) + ": " + detail;
	// a broken adapter would otherwise print every frame
	if (m_problem_count < 20)
		fprintf(stderr, "[retro_vk_engine] %s: %s\n", what, detail);
	else if (m_problem_count == 20)
		fprintf(stderr, "[retro_vk_engine] further problems are not logged\n");
	m_problem_count++;
}

bool retro_vk_engine::note_result(bool ok, int &streak, const char *what)
{
	if (ok)
	{
		streak = 0;
		return true;
	}
	if (m_first_refusal.empty())
	{
		m_first_refusal = m_last_problem.empty() ? std::string("no reason was logged") : m_last_problem;
		fprintf(stderr, "[retro_vk_engine] first refused frame (%s): %s\n", what, m_first_refusal.c_str());
	}
	if (++streak >= MAX_FAILED_FRAMES && !m_failed)
	{
		m_failed = true;
		fprintf(stderr, "[retro_vk_engine] %d %s failures in a row; the Vulkan engine is off for this session\n", streak, what);
	}
	return false;
}

int retro_vk_engine::texel_bytes(osd::gpu_handle texture) const
{
	return get_format_info(m_textures.find(texture)->second.desc.format).bytes;
}


//**************************************************************************
//  Images
//**************************************************************************

bool retro_vk_engine::create_image(image &img, VkFormat format, int width, int height, VkImageUsageFlags usage, VkImageAspectFlags aspect, int samples,
		VkImageCreateFlags flags)
{
	const VkDevice dev = m_device.device();

	VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	info.imageType = VK_IMAGE_TYPE_2D;
	info.format = format;
	info.extent.width = uint32_t(width);
	info.extent.height = uint32_t(height);
	info.extent.depth = 1;
	info.mipLevels = 1;
	info.arrayLayers = 1;
	info.samples = VkSampleCountFlagBits(samples);
	info.flags = flags;
	info.tiling = VK_IMAGE_TILING_OPTIMAL;
	info.usage = usage;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	if (m_device.fn.create_image(dev, &info, nullptr, &img.handle) != VK_SUCCESS)
		return false;

	VkMemoryRequirements req = {};
	m_device.fn.get_image_memory_requirements(dev, img.handle, &req);
	img.memory = m_device.allocate(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (img.memory == VK_NULL_HANDLE || m_device.fn.bind_image_memory(dev, img.handle, img.memory, 0) != VK_SUCCESS)
	{
		destroy_image(img);
		return false;
	}
	img.aspect = aspect;
	img.format = format;
	img.width = width;
	img.height = height;
	VkImageViewCreateInfo view = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
	view.image = img.handle;
	view.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view.format = format;
	view.subresourceRange = full_range(aspect);
	if (m_device.fn.create_image_view(dev, &view, nullptr, &img.view) != VK_SUCCESS)
	{
		destroy_image(img);
		return false;
	}
	return true;
}

void retro_vk_engine::destroy_image(image &img)
{
	const VkDevice dev = m_device.device();
	if (img.view != VK_NULL_HANDLE)
		m_device.fn.destroy_image_view(dev, img.view, nullptr);
	if (img.handle != VK_NULL_HANDLE)
		m_device.fn.destroy_image(dev, img.handle, nullptr);
	if (img.memory != VK_NULL_HANDLE)
		m_device.fn.free_memory(dev, img.memory, nullptr);
	img = image();
}

// Moves a new image from UNDEFINED to GENERAL in its own submission, so later frames can
// assume GENERAL without tracking layouts.
bool retro_vk_engine::transition_to_general(const image &img)
{
	if (!begin_recording())
		return false;
	VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
	barrier.srcAccessMask = 0;
	barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = img.handle;
	barrier.subresourceRange = full_range(img.aspect);
	m_device.fn.cmd_pipeline_barrier(m_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
			0, 0, nullptr, 0, nullptr, 1, &barrier);
	return end_recording(true);
}


//**************************************************************************
//  Textures
//**************************************************************************

osd::gpu_handle retro_vk_engine::create_texture(const osd::gpu_texture_desc &desc)
{
	if (failed() || desc.width <= 0 || desc.height <= 0
			|| desc.width > m_caps.max_texture_size || desc.height > m_caps.max_texture_size)
		return osd::GPU_NULL_HANDLE;

	const format_info fi = get_format_info(desc.format);
	const uint64_t bytes = uint64_t(desc.width) * desc.height * fi.bytes;
	if (m_texture_bytes + bytes > m_texture_budget)
	{
		log_problem("create_texture", "texture memory budget exceeded");
		return osd::GPU_NULL_HANDLE;
	}

	texture t;
	t.desc = desc;
	t.bytes = bytes;
	const VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	if (!create_image(t.img, fi.vk, desc.width, desc.height, usage, VK_IMAGE_ASPECT_COLOR_BIT) || !transition_to_general(t.img))
	{
		destroy_image(t.img);
		log_problem("create_texture", "image allocation failed");
		return osd::GPU_NULL_HANDLE;
	}

	m_texture_bytes += bytes;
	const osd::gpu_handle handle = m_next_handle++;
	m_textures.emplace(handle, t);
	return handle;
}

void retro_vk_engine::destroy_texture(osd::gpu_handle handle)
{
	const auto it = m_textures.find(handle);
	if (it == m_textures.end())
		return;
	// the frame in flight may still sample the image
	wait_in_flight();
	destroy_image(it->second.img);
	m_texture_bytes -= it->second.bytes;
	m_textures.erase(it);
}

bool retro_vk_engine::validate_update(const osd::gpu_texture_update &u)
{
	const auto it = m_textures.find(u.texture);
	if (it == m_textures.end())
	{
		log_problem("texture update", "unknown texture handle");
		return false;
	}
	const texture &t = it->second;
	if (!u.data || u.w <= 0 || u.h <= 0 || u.x < 0 || u.y < 0 || u.x + u.w > t.desc.width || u.y + u.h > t.desc.height)
	{
		log_problem("texture update", "rectangle outside the texture or no data");
		return false;
	}
	const int bpp = get_format_info(t.desc.format).bytes;
	if (u.pitch_bytes != 0 && (u.pitch_bytes % bpp != 0 || u.pitch_bytes < u.w * bpp))
	{
		log_problem("texture update", "pitch is not a whole number of texels or is smaller than a row");
		return false;
	}
	return true;
}

// Copies the rows of a texture update into the staging buffer, tightly packed.
void retro_vk_engine::write_update(const osd::gpu_texture_update &u, uint8_t *dst) const
{
	const size_t row_bytes = size_t(u.w) * texel_bytes(u.texture);
	const size_t pitch = u.pitch_bytes ? size_t(u.pitch_bytes) : row_bytes;
	const uint8_t *src = static_cast<const uint8_t *>(u.data);
	for (int row = 0; row < u.h; row++)
		memcpy(dst + size_t(row) * row_bytes, src + size_t(row) * pitch, row_bytes);
}


//**************************************************************************
//  Shader modules
//**************************************************************************

// Only registered here: pipelines are built by the draw path (VK.3c).
osd::gpu_handle retro_vk_engine::create_shader_module(const osd::gpu_shader_module &desc)
{
	if (failed() || !desc.fragment_source
			|| desc.varying_count < 0 || desc.varying_count > osd::GPU_MAX_VARYINGS
			|| desc.binding_count < 0 || desc.binding_count > osd::GPU_MAX_BINDINGS || desc.state_stride < 0)
		return osd::GPU_NULL_HANDLE;

	module m;
	m.name = desc.name;
	m.source = desc.fragment_source;
	m.centroid_mask = desc.centroid_mask;
	m.varying_count = desc.varying_count;
	std::copy(desc.varying_mode, desc.varying_mode + osd::GPU_MAX_VARYINGS, m.varying_mode);
	m.state_stride = desc.state_stride;
	m.writes_depth = desc.writes_depth;
	m.blend_factor_output = desc.blend_factor_output;
	m.binding_count = desc.binding_count;
	for (int i = 0; i < desc.binding_count; i++)
	{
		m.binding_name[i] = desc.binding_name[i] ? desc.binding_name[i] : "";
		m.binding_format[i] = desc.binding_format[i];
	}

	const osd::gpu_handle handle = m_next_handle++;
	m_modules.emplace(handle, std::move(m));
	return handle;
}

void retro_vk_engine::destroy_shader_module(osd::gpu_handle handle)
{
	if (m_device.valid())
	{
		// the frame in flight may still use the pipelines
		wait_in_flight();
		const VkDevice dev = m_device.device();
		for (auto it = m_pipelines.begin(); it != m_pipelines.end(); )
		{
			if (it->first.module == handle)
			{
				m_device.fn.destroy_pipeline(dev, it->second, nullptr);
				it = m_pipelines.erase(it);
			}
			else
				++it;
		}
		for (auto it = m_programs.begin(); it != m_programs.end(); )
		{
			if (it->first.first == handle)
			{
				m_device.fn.destroy_shader_module(dev, it->second.vertex, nullptr);
				m_device.fn.destroy_shader_module(dev, it->second.fragment, nullptr);
				it = m_programs.erase(it);
			}
			else
				++it;
		}
	}
	m_modules.erase(handle);
}


//**************************************************************************
//  Targets
//**************************************************************************

// Allocates the target when its size or depth mode changes, and reports whether
// the target must be cleared before this frame's commands (the same rules as the
// OpenGL engine's ensure_target()).
bool retro_vk_engine::ensure_target(const osd::gpu_target_desc &target, bool &clear_color, bool &clear_depth)
{
	const bool realloc = !m_target_valid || target.width != m_target.width || target.height != m_target.height
			|| target.depth != m_target.depth || target.msaa_samples != m_target.msaa_samples;

	clear_color = !target.persistent;
	clear_depth = target.depth == osd::gpu_depth_mode::ORDER || !target.persistent;

	if (realloc)
	{
		release_target();

		// a multisampled target is drawn at its sample count and resolved into m_resolve for the read-backs
		const int samples = std::max(target.msaa_samples, 1);
		const VkImageUsageFlags color_usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		bool ok = create_image(m_color, VK_FORMAT_B8G8R8A8_UNORM, target.width, target.height, color_usage, VK_IMAGE_ASPECT_COLOR_BIT, samples)
				&& create_image(m_scratch, VK_FORMAT_B8G8R8A8_UNORM, target.width, target.height, color_usage, VK_IMAGE_ASPECT_COLOR_BIT, samples)
				&& transition_to_general(m_color)
				&& transition_to_general(m_scratch);
		if (ok && target.msaa_samples > 0)
			ok = create_image(m_resolve, VK_FORMAT_B8G8R8A8_UNORM, target.width, target.height, color_usage, VK_IMAGE_ASPECT_COLOR_BIT)
					&& transition_to_general(m_resolve);
		if (ok && target.depth != osd::gpu_depth_mode::NONE)
		{
			const VkImageUsageFlags depth_usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
			// the depth values stay valid when draws use different sample locations (VK_EXT_sample_locations)
			const VkImageCreateFlags depth_flags = centre_samples(target.msaa_samples) ? VK_IMAGE_CREATE_SAMPLE_LOCATIONS_COMPATIBLE_DEPTH_BIT_EXT : 0;
			ok = create_image(m_depth, VK_FORMAT_D32_SFLOAT, target.width, target.height, depth_usage, VK_IMAGE_ASPECT_DEPTH_BIT, samples, depth_flags)
					&& transition_to_general(m_depth);
		}
		if (!ok)
		{
			release_target();
			log_problem("render target", "image allocation failed");
			return false;
		}
		if (target.msaa_samples > 0)
		{
			// the memory the target asks for: colour and depth for every sample, plus the single-sample resolve image
			const double pixels = double(target.width) * target.height;
			const double depth_bytes = (target.depth != osd::gpu_depth_mode::NONE) ? 4.0 : 0.0;
			fprintf(stderr, "[retro_vk_engine] target %dx%d, %d samples: colour+depth %.1f MB, resolve %.1f MB\n",
					target.width, target.height, target.msaa_samples, pixels * samples * (4.0 + depth_bytes) / 1048576.0, pixels * 4.0 / 1048576.0);
		}
		clear_color = true;
		clear_depth = true;
	}
	else if (target.persistent && target.generation != m_target.generation)
	{
		clear_color = true;
		clear_depth = true;
	}

	m_target = target;
	m_target_valid = true;
	return true;
}

void retro_vk_engine::release_target()
{
	destroy_image(m_color);
	destroy_image(m_scratch);
	destroy_image(m_depth);
	destroy_image(m_resolve);
	m_target_valid = false;
}


//**************************************************************************
//  Command recording
//**************************************************************************

// Records into m_cmd. The command pool is reset here, which is safe because every
// earlier submission has been waited for.
// The command buffer is reused, so the frame in flight (if any) must finish first.
bool retro_vk_engine::begin_recording()
{
	if (!wait_in_flight())
		return false;
	if (m_device.fn.reset_command_pool(m_device.device(), m_device.command_pool(), 0) != VK_SUCCESS)
		return false;
	m_locations_mode = -1;
	VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	return m_device.fn.begin_command_buffer(m_cmd, &begin) == VK_SUCCESS;
}

// Submits what was recorded. With wait the call returns once the GPU has finished it; otherwise the frame stays in
// flight until wait_in_flight() is called, which every later call that reuses the command buffer does first.
bool retro_vk_engine::end_recording(bool wait)
{
	const VkDevice dev = m_device.device();
	if (m_device.fn.end_command_buffer(m_cmd) != VK_SUCCESS)
		return false;
	m_device.fn.reset_fences(dev, 1, &m_fence);
	VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &m_cmd;
	if (m_device.fn.queue_submit(m_device.queue(), 1, &submit, m_fence) != VK_SUCCESS)
		return false;
	m_in_flight = true;
	return wait ? wait_in_flight() : true;
}

// Waits for the frame in flight, if there is one, and reads its timestamps.
bool retro_vk_engine::wait_in_flight()
{
	if (!m_in_flight)
		return true;
	m_in_flight = false;
	if (m_device.fn.wait_for_fences(m_device.device(), 1, &m_fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS)
	{
		log_problem("frame", "waiting for the GPU failed");
		return false;
	}
	if (m_query_pending)
		read_timer();
	return true;
}

void retro_vk_engine::read_timer()
{
	uint64_t ts[2] = { 0, 0 };
	if (m_device.fn.get_query_pool_results(m_device.device(), m_query_pool, 0, 2, sizeof(ts), ts, sizeof(uint64_t),
			VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS)
	{
		const uint64_t ticks = (ts[1] - ts[0]) & m_timestamp_mask;
		m_last_gpu_ms = double(ticks) * m_timestamp_period * 1e-6;
		m_gpu_ms_ready = true;
	}
	m_query_pending = false;
}

// Orders each transfer or clear after the one before it. The images are small and the
// frame is short, so a full barrier is simpler than tracking dependencies per resource.
void retro_vk_engine::record_barrier()
{
	VkMemoryBarrier barrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
	barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	m_device.fn.cmd_pipeline_barrier(m_cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
			0, 1, &barrier, 0, nullptr, 0, nullptr);
}

// CLEAR and the target's own clear ignore the clip, as the OpenGL engine's do.
void retro_vk_engine::record_clear(const float *rgba, bool color, bool depth)
{
	if (m_target.msaa_samples > 0)
	{
		record_clear_multisample(rgba, color, depth);
		return;
	}
	if (color)
	{
		VkClearColorValue value = {};
		for (int i = 0; i < 4; i++)
			value.float32[i] = rgba[i];
		const VkImageSubresourceRange range = full_range(VK_IMAGE_ASPECT_COLOR_BIT);
		m_device.fn.cmd_clear_color_image(m_cmd, m_color.handle, VK_IMAGE_LAYOUT_GENERAL, &value, 1, &range);
	}
	if (depth && m_depth.handle != VK_NULL_HANDLE)
	{
		VkClearDepthStencilValue value = {};
		value.depth = 1.0f;
		const VkImageSubresourceRange range = full_range(VK_IMAGE_ASPECT_DEPTH_BIT);
		m_device.fn.cmd_clear_depth_stencil_image(m_cmd, m_depth.handle, VK_IMAGE_LAYOUT_GENERAL, &value, 1, &range);
	}
}

// The standard sample pattern for 2, 4 and 8 samples, which an enabled pipeline uses when a draw names the standard
// locations (the same positions a pipeline without custom locations gets).
constexpr VkSampleLocationEXT STANDARD_LOCATIONS_2[] = { { 0.25f, 0.25f }, { 0.75f, 0.75f } };
constexpr VkSampleLocationEXT STANDARD_LOCATIONS_4[] = { { 0.375f, 0.125f }, { 0.875f, 0.375f }, { 0.125f, 0.625f }, { 0.625f, 0.875f } };
constexpr VkSampleLocationEXT STANDARD_LOCATIONS_8[] = {
		{ 0.5625f, 0.3125f }, { 0.4375f, 0.6875f }, { 0.8125f, 0.5625f }, { 0.3125f, 0.1875f },
		{ 0.1875f, 0.8125f }, { 0.0625f, 0.4375f }, { 0.6875f, 0.9375f }, { 0.9375f, 0.0625f } };

// Sets the sample locations for the draws that follow on a multisample target: every sample at the pixel centre for a
// draw without multisampling, which covers the pixels whose centres the triangle contains and writes all their samples
// (the OpenGL engine's GL_MULTISAMPLE-off draw), and the standard pattern otherwise. Only called when centre_samples().
void retro_vk_engine::set_sample_locations(bool multisample)
{
	const int mode = multisample ? 0 : 1;
	if (mode == m_locations_mode)
		return;
	m_locations_mode = mode;
	const int samples = m_target.msaa_samples;
	const VkSampleLocationEXT centre[8] = { { 0.5f, 0.5f }, { 0.5f, 0.5f }, { 0.5f, 0.5f }, { 0.5f, 0.5f },
			{ 0.5f, 0.5f }, { 0.5f, 0.5f }, { 0.5f, 0.5f }, { 0.5f, 0.5f } };
	const VkSampleLocationEXT *pattern = centre;
	if (multisample)
		pattern = (samples == 2) ? STANDARD_LOCATIONS_2 : (samples == 4) ? STANDARD_LOCATIONS_4 : STANDARD_LOCATIONS_8;

	VkSampleLocationsInfoEXT info = { VK_STRUCTURE_TYPE_SAMPLE_LOCATIONS_INFO_EXT };
	info.sampleLocationsPerPixel = VkSampleCountFlagBits(samples);
	info.sampleLocationGridSize.width = 1;
	info.sampleLocationGridSize.height = 1;
	info.sampleLocationsCount = uint32_t(samples);
	info.pSampleLocations = pattern;
	m_device.fn.cmd_set_sample_locations(m_cmd, &info);
}

// A multisampled image cannot be cleared by vkCmdClearColorImage, so the clear is the load operation of an empty render
// pass. Attachments that are not cleared keep their contents.
void retro_vk_engine::record_clear_multisample(const float *rgba, bool color, bool depth)
{
	VkRenderingAttachmentInfo colour = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
	colour.imageView = m_color.view;
	colour.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
	colour.loadOp = color ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
	colour.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	for (int i = 0; i < 4; i++)
		colour.clearValue.color.float32[i] = rgba[i];
	VkRenderingAttachmentInfo depth_attachment = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
	depth_attachment.imageView = m_depth.view;
	depth_attachment.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
	depth_attachment.loadOp = depth ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
	depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	depth_attachment.clearValue.depthStencil.depth = 1.0f;

	VkRenderingInfo info = { VK_STRUCTURE_TYPE_RENDERING_INFO };
	info.renderArea.extent.width = uint32_t(m_target.width);
	info.renderArea.extent.height = uint32_t(m_target.height);
	info.layerCount = 1;
	info.colorAttachmentCount = 1;
	info.pColorAttachments = &colour;
	info.pDepthAttachment = (m_depth.handle != VK_NULL_HANDLE) ? &depth_attachment : nullptr;
	m_device.fn.cmd_begin_rendering(m_cmd, &info);
	m_device.fn.cmd_end_rendering(m_cmd);
}

// Averages the samples of the colour target into the single-sample image the read-backs read.
void retro_vk_engine::record_resolve()
{
	VkImageResolve region = {};
	region.srcSubresource = layers(VK_IMAGE_ASPECT_COLOR_BIT);
	region.dstSubresource = layers(VK_IMAGE_ASPECT_COLOR_BIT);
	region.extent.width = uint32_t(m_target.width);
	region.extent.height = uint32_t(m_target.height);
	region.extent.depth = 1;
	m_device.fn.cmd_resolve_image(m_cmd, m_color.handle, VK_IMAGE_LAYOUT_GENERAL, m_resolve.handle, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
	record_barrier();
}

// Copies through the scratch image, so the source is the target as it was before the command.
void retro_vk_engine::record_copy(const osd::gpu_command &c)
{
	VkImageCopy region = {};
	region.srcSubresource = layers(VK_IMAGE_ASPECT_COLOR_BIT);
	region.dstSubresource = layers(VK_IMAGE_ASPECT_COLOR_BIT);
	region.extent.width = uint32_t(c.w);
	region.extent.height = uint32_t(c.h);
	region.extent.depth = 1;

	region.srcOffset.x = c.x;
	region.srcOffset.y = c.y;
	m_device.fn.cmd_copy_image(m_cmd, m_color.handle, VK_IMAGE_LAYOUT_GENERAL, m_scratch.handle, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
	record_barrier();

	region.srcOffset = {};
	region.dstOffset.x = c.dx;
	region.dstOffset.y = c.dy;
	m_device.fn.cmd_copy_image(m_cmd, m_scratch.handle, VK_IMAGE_LAYOUT_GENERAL, m_color.handle, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
}

void retro_vk_engine::record_update(const osd::gpu_texture_update &u, size_t offset, const staging_buffer &staging)
{
	const texture &t = m_textures.find(u.texture)->second;
	VkBufferImageCopy region = {};
	region.bufferOffset = offset;
	region.imageSubresource = layers(VK_IMAGE_ASPECT_COLOR_BIT);
	region.imageOffset.x = u.x;
	region.imageOffset.y = u.y;
	region.imageExtent.width = uint32_t(u.w);
	region.imageExtent.height = uint32_t(u.h);
	region.imageExtent.depth = 1;
	m_device.fn.cmd_copy_buffer_to_image(m_cmd, staging.buffer, t.img.handle, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
}

// Read-back copies BGRA8 bytes, four per target pixel, tightly packed into the staging buffer.
void retro_vk_engine::record_readback(const osd::gpu_readback_rect &r, size_t offset, const staging_buffer &staging)
{
	if (m_target.msaa_samples > 0)
		record_resolve();
	VkBufferImageCopy region = {};
	region.bufferOffset = offset;
	region.imageSubresource = layers(VK_IMAGE_ASPECT_COLOR_BIT);
	region.imageOffset.x = r.x;
	region.imageOffset.y = r.y;
	region.imageExtent.width = uint32_t(r.w);
	region.imageExtent.height = uint32_t(r.h);
	region.imageExtent.depth = 1;
	m_device.fn.cmd_copy_image_to_buffer(m_cmd, readback_image().handle, VK_IMAGE_LAYOUT_GENERAL, staging.buffer, 1, &region);
}

// A read-back region holds its lookup table first (readback_lut_bytes), then its pixels: the rectangle as BGRA bytes
// for a plain read-back, or the output pixels of the read-back pass, one per ds-by-ds block.
size_t retro_vk_engine::readback_lut_bytes(const osd::gpu_readback_rect &r) const
{
	return (!r.plain() && r.channel_lut) ? 128 * 4 : 0;
}

size_t retro_vk_engine::readback_bytes(const osd::gpu_readback_rect &r) const
{
	const size_t ds = size_t(std::max(r.downsample, 1));
	const size_t pixels = r.plain() ? size_t(r.w) * r.h : (size_t(r.w) / ds) * (size_t(r.h) / ds);
	return readback_lut_bytes(r) + pixels * 4;
}

// Each lookup value is stored as one word, so the pass reads it with the same indexing as the OpenGL uniform array.
void retro_vk_engine::write_readback_lut(const osd::gpu_readback_rect &r, uint8_t *dst) const
{
	if (!readback_lut_bytes(r))
		return;
	for (int i = 0; i < 128; i++)
	{
		const uint32_t value = r.channel_lut[i];
		memcpy(dst + size_t(i) * 4, &value, sizeof(value));
	}
}

// Writes the pixels of a read-back: BGRA bytes for a plain read-back, or 8-bit red and green per pixel for RG8.
// A plain read-back copies the rectangle; any other runs the read-back pass.
bool retro_vk_engine::record_readback_any(const osd::gpu_readback_rect &r, size_t at, const staging_buffer &staging)
{
	if (r.plain())
	{
		record_readback(r, at, staging);
		return true;
	}
	return record_output_pass(r, at, staging);
}

// Records the read-back pass: a compute dispatch over the output pixels, reading the target in its current layout and
// writing into the staging buffer. Its descriptor set is taken from the frame's pools and written now, which is safe
// because the frame's command buffer is only executed after this call returns.
bool retro_vk_engine::record_output_pass(const osd::gpu_readback_rect &r, size_t at, const staging_buffer &staging)
{
	const bool per_sample = reads_samples(r);
	if (!ensure_output_pipeline(per_sample))
		return false;
	if (m_target.msaa_samples > 0 && !per_sample)
		record_resolve();
	VkDescriptorSet set = VK_NULL_HANDLE;
	if (!allocate_frame_set(set))
	{
		log_problem("output pass", "descriptor set allocation failed");
		return false;
	}

	const VkDevice dev = m_device.device();
	VkDescriptorBufferInfo buffer = { staging.buffer, 0, VK_WHOLE_SIZE };
	const image &source = per_sample ? m_color : readback_image();
	VkDescriptorImageInfo image = { m_sampler_nearest, source.view, VK_IMAGE_LAYOUT_GENERAL };
	VkWriteDescriptorSet writes[2] = {};
	writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[0].dstSet = set;
	writes[0].dstBinding = 0;
	writes[0].descriptorCount = 1;
	writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	writes[0].pBufferInfo = &buffer;
	writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[1].dstSet = set;
	writes[1].dstBinding = 1;
	writes[1].descriptorCount = 1;
	writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	writes[1].pImageInfo = &image;
	m_device.fn.update_descriptor_sets(dev, 2, writes, 0, nullptr);

	const int ds = std::max(r.downsample, 1);
	const size_t lut = readback_lut_bytes(r);
	output_constants pc = {};
	pc.origin[0] = r.x;
	pc.origin[1] = r.y;
	pc.size[0] = r.w / ds;
	pc.size[1] = r.h / ds;
	pc.ds = ds;
	pc.flags = (r.channel_lut ? 1 : 0) | (r.opaque ? 2 : 0) | (r.coverage ? 4 : 0) | (r.coverage_share ? 8 : 0) | (r.priority_bits ? 16 : 0);
	pc.out_at = int32_t((at + lut) / 4);
	pc.lut_at = int32_t(at / 4);
	pc.samples = per_sample ? m_target.msaa_samples : 1;

	m_device.fn.cmd_bind_pipeline(m_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_output_pipeline[per_sample ? 1 : 0]);
	m_device.fn.cmd_bind_descriptor_sets(m_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_output_layout, 0, 1, &set, 0, nullptr);
	m_device.fn.cmd_push_constants(m_cmd, m_output_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
	// the target's writes (from the draws or a copy) are visible to the pass
	record_barrier();
	m_device.fn.cmd_dispatch(m_cmd, uint32_t((pc.size[0] + 7) / 8), uint32_t((pc.size[1] + 7) / 8), 1);
	return true;
}

// The read-back pass for one kind of read, built on first use and kept for the session. It binds the frame's staging
// buffer and the target. The layout is shared by both kinds.
bool retro_vk_engine::ensure_output_pipeline(bool per_sample)
{
	const size_t kind = per_sample ? 1 : 0;
	if (m_output_pipeline[kind] != VK_NULL_HANDLE)
		return true;
	const VkDevice dev = m_device.device();
	std::vector<uint32_t> spirv;
	std::string log;
	const std::string source = output_source(per_sample);
	if (!retro_vk::compile_cached(retro_vk::shader_stage::COMPUTE, source, retro_vk::shader_cache_key(device_key()), m_cache_dir, spirv, log))
	{
		log_problem("output pass", log.c_str());
		return false;
	}

	if (m_output_layout == VK_NULL_HANDLE)
	{
		VkPushConstantRange push = {};
		push.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
		push.size = sizeof(output_constants);
		VkPipelineLayoutCreateInfo layout_info = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
		layout_info.setLayoutCount = 1;
		layout_info.pSetLayouts = &m_set_layout;
		layout_info.pushConstantRangeCount = 1;
		layout_info.pPushConstantRanges = &push;
		if (m_device.fn.create_pipeline_layout(dev, &layout_info, nullptr, &m_output_layout) != VK_SUCCESS)
		{
			log_problem("output pass", "layout creation failed");
			return false;
		}
	}

	VkShaderModuleCreateInfo module_info = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
	module_info.codeSize = spirv.size() * sizeof(uint32_t);
	module_info.pCode = spirv.data();
	if (m_device.fn.create_shader_module(dev, &module_info, nullptr, &m_output_module[kind]) != VK_SUCCESS)
	{
		log_problem("output pass", "module creation failed");
		return false;
	}
	VkComputePipelineCreateInfo pipeline_info = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
	pipeline_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	pipeline_info.stage.pName = "main";
	pipeline_info.stage.module = m_output_module[kind];
	pipeline_info.layout = m_output_layout;
	if (m_device.fn.create_compute_pipelines(dev, m_pipeline_cache, 1, &pipeline_info, nullptr, &m_output_pipeline[kind]) != VK_SUCCESS)
	{
		log_problem("output pass", "pipeline creation failed");
		return false;
	}
	return true;
}

void retro_vk_engine::unpack_readback(const osd::gpu_readback_rect &r, const uint8_t *src) const
{
	// a read-back that is not plain has one output pixel per ds-by-ds block of the target
	const int ds = std::max(r.downsample, 1);
	const int ow = r.w / ds;
	const int oh = r.h / ds;
	const size_t row_bytes = size_t(ow) * 4;
	for (int row = 0; row < oh; row++)
	{
		const uint8_t *line = src + size_t(row) * row_bytes;
		if (r.format == osd::gpu_readback_format::RG8)
		{
			// the red and green bytes of each BGRA pixel, as GL_RG / GL_UNSIGNED_BYTE gives them
			uint16_t *out = r.out16 + size_t(row) * r.stride;
			for (int col = 0; col < ow; col++)
				out[col] = uint16_t(line[col * 4 + 2] | (line[col * 4 + 1] << 8));
		}
		else
		{
			uint8_t *out = reinterpret_cast<uint8_t *>(r.out) + size_t(row) * r.stride * 4;
			memcpy(out, line, row_bytes);
		}
	}
}


//**************************************************************************
//  Staging
//**************************************************************************

bool retro_vk_engine::create_staging(size_t bytes, staging_buffer &staging)
{
	const VkDevice dev = m_device.device();
	VkBufferCreateInfo info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	info.size = bytes;
	// one buffer per frame holds the uploads, the read-backs, the vertices and the state records
	info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT
			| VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	if (m_device.fn.create_buffer(dev, &info, nullptr, &staging.buffer) != VK_SUCCESS)
		return false;

	VkMemoryRequirements req = {};
	m_device.fn.get_buffer_memory_requirements(dev, staging.buffer, &req);
	staging.memory = m_device.allocate(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	void *mapped = nullptr;
	if (staging.memory == VK_NULL_HANDLE
			|| m_device.fn.bind_buffer_memory(dev, staging.buffer, staging.memory, 0) != VK_SUCCESS
			|| m_device.fn.map_memory(dev, staging.memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS)
	{
		destroy_staging(staging);
		return false;
	}
	staging.data = static_cast<uint8_t *>(mapped);
	return true;
}

void retro_vk_engine::destroy_staging(staging_buffer &staging)
{
	const VkDevice dev = m_device.device();
	if (staging.data)
		m_device.fn.unmap_memory(dev, staging.memory);
	if (staging.buffer != VK_NULL_HANDLE)
		m_device.fn.destroy_buffer(dev, staging.buffer, nullptr);
	if (staging.memory != VK_NULL_HANDLE)
		m_device.fn.free_memory(dev, staging.memory, nullptr);
	staging = staging_buffer();
}


//**************************************************************************
//  Frames
//**************************************************************************

bool retro_vk_engine::sample_count_supported(int samples, bool sampled) const
{
	return samples >= 1 && ((sampled ? m_sampled_sample_counts : m_sample_counts) & uint32_t(samples)) != 0;
}

// Reduces the requested sample count to what the device supports and to the memory budget for the target (colour and
// depth samples plus the single-sample resolve image), as the OpenGL engine's fit_msaa() does. One sample is not
// multisampling, so it becomes none.
void retro_vk_engine::fit_msaa(osd::gpu_target_desc &target)
{
	const int requested = target.msaa_samples;
	int samples = std::min(requested, m_caps.max_samples);
	while (samples >= 2 && !sample_count_supported(samples, target.msaa_per_sample))
		samples /= 2;

	const uint64_t pixels = uint64_t(std::max(target.width, 0)) * std::max(target.height, 0);
	const uint64_t sample_bytes = (target.depth != osd::gpu_depth_mode::NONE) ? 8 : 4;
	while (samples >= 2 && pixels * samples * sample_bytes + pixels * 4 > m_msaa_budget)
	{
		samples /= 2;
		while (samples >= 2 && !sample_count_supported(samples, target.msaa_per_sample))
			samples /= 2;
	}
	target.msaa_samples = (samples < 2) ? 0 : samples;

	if (requested != target.msaa_samples && (requested != m_msaa_logged_requested || target.msaa_samples != m_msaa_logged_granted))
	{
		m_msaa_logged_requested = requested;
		m_msaa_logged_granted = target.msaa_samples;
		fprintf(stderr, "[retro_vk_engine] %dx MSAA on a %dx%d target reduced to %d samples (device limit %d, memory budget %llu MB)\n",
				requested, target.width, target.height, target.msaa_samples, m_caps.max_samples, (unsigned long long)(m_msaa_budget >> 20));
	}
}

// A read-back of a multisample target reads the samples one by one when the target keeps them readable and the read-back
// needs them (coverage or priority bits); any other read-back reads the resolved image.
bool retro_vk_engine::reads_samples(const osd::gpu_readback_rect &r) const
{
	return m_target.msaa_samples > 0 && m_target.msaa_per_sample && (r.coverage || r.priority_bits);
}

// Overlay triangles drawn without multisampling are moved to pixel centres, so each covers the pixels whose centres it
// contains rather than a sliver of samples. A triangle whose three vertices all sit on whole pixels (the pixel-aligned
// rectangles) is left as it is. The vertices are the frame's copy in the staging buffer.
void retro_vk_engine::snap_triangles(const osd::gpu_frame &frame, uint8_t *vertices) const
{
	const size_t stride = size_t(5 + frame.varyings_per_vertex) * 4;
	for (int i = 0; i < frame.command_count; i++)
	{
		const osd::gpu_command &c = frame.commands[i];
		if (c.type != osd::gpu_command_type::DRAW || c.multisample)
			continue;
		for (uint32_t t = 0; t + 3 <= c.vertex_count; t += 3)
		{
			float *v[3] = {};
			bool whole = true;
			for (int k = 0; k < 3; k++)
			{
				v[k] = reinterpret_cast<float *>(vertices + (size_t(c.first_vertex) + t + k) * stride);
				whole = whole && v[k][0] == std::floor(v[k][0]) && v[k][1] == std::floor(v[k][1]);
			}
			if (whole)
				continue;
			for (int k = 0; k < 3; k++)
			{
				v[k][0] = std::floor(v[k][0]) + 0.5f;
				v[k][1] = std::floor(v[k][1]) + 0.5f;
			}
		}
	}
}

// Refuses the frames the OpenGL engine refuses, with the same checks.
bool retro_vk_engine::validate_frame(const osd::gpu_frame &frame)
{
	const osd::gpu_target_desc &tg = frame.target;
	if (tg.width <= 0 || tg.height <= 0 || tg.width > m_caps.max_texture_size || tg.height > m_caps.max_texture_size)
	{
		log_problem("frame", "target size out of range");
		return false;
	}
	if (frame.update_count > 0 && !frame.updates)
	{
		log_problem("frame", "no texture updates");
		return false;
	}
	if (frame.command_count > 0 && !frame.commands)
	{
		log_problem("frame", "no command list");
		return false;
	}
	if (frame.varyings_per_vertex < 0 || frame.varyings_per_vertex > osd::GPU_MAX_VARYINGS)
	{
		log_problem("frame", "varyings_per_vertex out of range");
		return false;
	}
	if (frame.vertex_count > 0 && !frame.vertices)
	{
		log_problem("frame", "no vertex data");
		return false;
	}
	if (frame.prim_count > 0 && frame.state_stride > 0 && !frame.prim_state)
	{
		log_problem("frame", "no primitive state data");
		return false;
	}

	for (int i = 0; i < frame.update_count; i++)
		if (!validate_update(frame.updates[i]))
			return false;

	for (int i = 0; i < frame.command_count; i++)
	{
		const osd::gpu_command &c = frame.commands[i];
		switch (c.type)
		{
		case osd::gpu_command_type::DRAW:
		{
			const auto mod = m_modules.find(c.module);
			if (mod == m_modules.end())
			{
				log_problem("draw", "unknown shader module");
				return false;
			}
			const module &m = mod->second;
			if (m.varying_count > frame.varyings_per_vertex || m.state_stride > frame.state_stride)
			{
				log_problem("draw", "module needs more varyings or state words than the frame provides");
				return false;
			}
			if (uint64_t(c.first_vertex) + c.vertex_count > frame.vertex_count || c.vertex_count % 3 != 0)
			{
				log_problem("draw", "vertex range outside the frame or not a whole number of triangles");
				return false;
			}
			for (int b = 0; b < m.binding_count; b++)
			{
				const auto tex = m_textures.find(c.bindings[b]);
				if (tex == m_textures.end() || tex->second.desc.format != m.binding_format[b])
				{
					log_problem("draw", "binding missing or has the wrong format");
					return false;
				}
			}
			break;
		}
		case osd::gpu_command_type::COPY_RECT:
			if (c.w <= 0 || c.h <= 0 || c.x < 0 || c.y < 0 || c.dx < 0 || c.dy < 0
					|| c.x + c.w > tg.width || c.y + c.h > tg.height || c.dx + c.w > tg.width || c.dy + c.h > tg.height)
			{
				log_problem("copy", "rectangle outside the target");
				return false;
			}
			break;
		case osd::gpu_command_type::UPDATE_TEXTURE:
			if (!validate_update(c.update))
				return false;
			break;
		case osd::gpu_command_type::READBACK:
			if (!valid_readback(c.readback, tg))
				return false;
			break;
		case osd::gpu_command_type::SET_CLIP:
		case osd::gpu_command_type::CLEAR:
			break;
		}
	}

	for (int i = 0; i < frame.readback_count; i++)
		if (!valid_readback(frame.readbacks[i], tg))
			return false;
	return true;
}

bool retro_vk_engine::valid_readback(const osd::gpu_readback_rect &r, const osd::gpu_target_desc &tg)
{
	const int ds = std::max(r.downsample, 1);
	const bool rg8 = r.format == osd::gpu_readback_format::RG8;
	if ((rg8 ? (!r.out16 || !r.plain()) : !r.out) || r.w <= 0 || r.h <= 0 || r.x < 0 || r.y < 0 || r.x + r.w > tg.width || r.y + r.h > tg.height
			|| r.w % ds != 0 || r.h % ds != 0 || r.stride < r.w / ds)
	{
		log_problem("readback", "rectangle outside the target, bad buffer, or a size that is not a multiple of the downsample");
		return false;
	}
	return true;
}

bool retro_vk_engine::submit_frame(const osd::gpu_frame &frame)
{
	if (failed())
		return false;
	return note_result(submit_pending(frame), m_submit_failures, "submit");
}


// Creates the samplers, the descriptor set layout, the pipeline layout and the pipeline cache. The layouts are shared
// by every module: binding 0 is the state buffer and bindings 1 to GPU_MAX_BINDINGS are the module's samplers.
bool retro_vk_engine::create_pipeline_objects()
{
	const VkDevice dev = m_device.device();
	m_cache_dir = retro_system_directory ? std::string(retro_system_directory) + "/mame/gpu-cache" : std::string();

	VkSamplerCreateInfo sampler = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
	sampler.magFilter = VK_FILTER_NEAREST;
	sampler.minFilter = VK_FILTER_NEAREST;
	sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	if (m_device.fn.create_sampler(dev, &sampler, nullptr, &m_sampler_nearest) != VK_SUCCESS)
		return false;
	sampler.magFilter = VK_FILTER_LINEAR;
	sampler.minFilter = VK_FILTER_LINEAR;
	if (m_device.fn.create_sampler(dev, &sampler, nullptr, &m_sampler_linear) != VK_SUCCESS)
		return false;

	VkDescriptorSetLayoutBinding bindings[1 + osd::GPU_MAX_BINDINGS] = {};
	bindings[0].binding = 0;
	bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	bindings[0].descriptorCount = 1;
	bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	for (int i = 0; i < osd::GPU_MAX_BINDINGS; i++)
	{
		bindings[1 + i].binding = uint32_t(1 + i);
		bindings[1 + i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		bindings[1 + i].descriptorCount = 1;
		bindings[1 + i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	}
	VkDescriptorSetLayoutCreateInfo set_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
	set_info.bindingCount = uint32_t(1 + osd::GPU_MAX_BINDINGS);
	set_info.pBindings = bindings;
	if (m_device.fn.create_descriptor_set_layout(dev, &set_info, nullptr, &m_set_layout) != VK_SUCCESS)
		return false;

	VkPushConstantRange push = {};
	push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
	push.size = sizeof(push_constants);
	VkPipelineLayoutCreateInfo layout_info = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	layout_info.setLayoutCount = 1;
	layout_info.pSetLayouts = &m_set_layout;
	layout_info.pushConstantRangeCount = 1;
	layout_info.pPushConstantRanges = &push;
	if (m_device.fn.create_pipeline_layout(dev, &layout_info, nullptr, &m_pipeline_layout) != VK_SUCCESS)
		return false;

	load_pipeline_cache();
	return true;
}

// Frees the pipelines, shader modules, descriptor pools and layouts, saving the pipeline cache first. Nothing is in
// flight when the engine is destroyed, so the objects can go at once.
void retro_vk_engine::destroy_pipeline_objects()
{
	if (!m_device.valid())
		return;
	const VkDevice dev = m_device.device();
	save_pipeline_cache();
	for (auto &p : m_pipelines)
		m_device.fn.destroy_pipeline(dev, p.second, nullptr);
	m_pipelines.clear();
	for (auto &p : m_programs)
	{
		m_device.fn.destroy_shader_module(dev, p.second.vertex, nullptr);
		m_device.fn.destroy_shader_module(dev, p.second.fragment, nullptr);
	}
	m_programs.clear();
	for (VkDescriptorPool pool : m_desc_pools)
		m_device.fn.destroy_descriptor_pool(dev, pool, nullptr);
	m_desc_pools.clear();
	m_desc_pool_next = 0;
	m_device.fn.destroy_pipeline_cache(dev, m_pipeline_cache, nullptr);
	for (size_t kind = 0; kind < 2; kind++)
	{
		m_device.fn.destroy_pipeline(dev, m_output_pipeline[kind], nullptr);
		m_device.fn.destroy_shader_module(dev, m_output_module[kind], nullptr);
		m_output_pipeline[kind] = VK_NULL_HANDLE;
		m_output_module[kind] = VK_NULL_HANDLE;
	}
	m_device.fn.destroy_pipeline_layout(dev, m_output_layout, nullptr);
	m_output_layout = VK_NULL_HANDLE;
	m_device.fn.destroy_pipeline_layout(dev, m_pipeline_layout, nullptr);
	m_device.fn.destroy_descriptor_set_layout(dev, m_set_layout, nullptr);
	m_device.fn.destroy_sampler(dev, m_sampler_linear, nullptr);
	m_device.fn.destroy_sampler(dev, m_sampler_nearest, nullptr);
	m_pipeline_cache = VK_NULL_HANDLE;
	m_pipeline_layout = VK_NULL_HANDLE;
	m_set_layout = VK_NULL_HANDLE;
	m_sampler_linear = VK_NULL_HANDLE;
	m_sampler_nearest = VK_NULL_HANDLE;
}

// The pipeline cache is loaded only when its header names this device and driver. An empty cache is used otherwise,
// and a cache that fails to create is left out: nothing depends on it.
void retro_vk_engine::load_pipeline_cache()
{
	std::vector<uint8_t> data;
	if (!m_cache_dir.empty())
	{
		std::ifstream in(m_cache_dir + "/" + PIPELINE_CACHE_NAME, std::ios::binary);
		data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
	}
	if (!pipeline_cache_usable(data, m_device.properties()))
		data.clear();

	VkPipelineCacheCreateInfo info = { VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
	info.initialDataSize = data.size();
	info.pInitialData = data.empty() ? nullptr : data.data();
	if (m_device.fn.create_pipeline_cache(m_device.device(), &info, nullptr, &m_pipeline_cache) != VK_SUCCESS)
		m_pipeline_cache = VK_NULL_HANDLE;
}

// Writes the cache through a temporary file, so the file in place is always complete. A failure keeps the old file.
void retro_vk_engine::save_pipeline_cache()
{
	if (!m_pipeline_cache_dirty || m_pipeline_cache == VK_NULL_HANDLE || m_cache_dir.empty())
		return;
	const VkDevice dev = m_device.device();
	size_t size = 0;
	if (m_device.fn.get_pipeline_cache_data(dev, m_pipeline_cache, &size, nullptr) != VK_SUCCESS || size == 0)
		return;
	std::vector<uint8_t> data(size);
	if (m_device.fn.get_pipeline_cache_data(dev, m_pipeline_cache, &size, data.data()) != VK_SUCCESS)
		return;
	data.resize(size);

	mkdir(m_cache_dir.c_str(), 0755);
	const std::string path = m_cache_dir + "/" + PIPELINE_CACHE_NAME;
	const std::string temp = path + ".tmp";
	{
		std::ofstream out(temp, std::ios::binary | std::ios::trunc);
		out.write(reinterpret_cast<const char *>(data.data()), std::streamsize(data.size()));
		if (!out)
		{
			out.close();
			std::remove(temp.c_str());
			return;
		}
	}
	if (std::rename(temp.c_str(), path.c_str()) == 0)
		m_pipeline_cache_dirty = false;
	else
		std::remove(temp.c_str());
}

// The device part of the shader cache key: vendor, device and driver version. A driver update changes the key.
std::string retro_vk_engine::device_key() const
{
	const VkPhysicalDeviceProperties &props = m_device.properties();
	return "vendor" + std::to_string(props.vendorID) + "-device" + std::to_string(props.deviceID)
			+ "-driver" + std::to_string(props.driverVersion);
}

// The shader pair for one module and variant, compiled on first use. A compile failure latches the engine: the module
// is not retried and the device returns to software for the session.
const retro_vk_engine::program *retro_vk_engine::get_program(osd::gpu_handle module_handle, uint32_t variant)
{
	const auto key = std::make_pair(module_handle, variant);
	const auto found = m_programs.find(key);
	if (found != m_programs.end())
		return &found->second;

	const module &m = m_modules.find(module_handle)->second;
	osd::gpu_shader_module desc;
	desc.name = m.name.c_str();
	desc.fragment_source = m.source.c_str();
	desc.centroid_mask = m.centroid_mask;
	desc.varying_count = m.varying_count;
	std::copy(m.varying_mode, m.varying_mode + osd::GPU_MAX_VARYINGS, desc.varying_mode);
	desc.state_stride = m.state_stride;
	desc.writes_depth = m.writes_depth;
	desc.blend_factor_output = m.blend_factor_output;
	desc.binding_count = m.binding_count;
	for (int i = 0; i < m.binding_count; i++)
	{
		desc.binding_name[i] = m.binding_name[i].c_str();
		desc.binding_format[i] = m.binding_format[i];
	}

	const std::string cache_key = retro_vk::shader_cache_key(device_key());
	const retro_vk::shader_stage stages[2] = { retro_vk::shader_stage::VERTEX, retro_vk::shader_stage::FRAGMENT };
	program p;
	VkShaderModule *const targets[2] = { &p.vertex, &p.fragment };
	bool ok = true;
	for (int s = 0; s < 2 && ok; s++)
	{
		const std::string source = (s == 0) ? retro_vk::vertex_glsl(desc) : retro_vk::fragment_glsl(desc, variant);
		std::vector<uint32_t> spirv;
		std::string log;
		if (!retro_vk::compile_cached(stages[s], source, cache_key, m_cache_dir, spirv, log))
		{
			const std::string detail = m.name + ": " + log;
			log_problem("shader", detail.c_str());
			ok = false;
			break;
		}
		VkShaderModuleCreateInfo info = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
		info.codeSize = spirv.size() * sizeof(uint32_t);
		info.pCode = spirv.data();
		ok = m_device.fn.create_shader_module(m_device.device(), &info, nullptr, targets[s]) == VK_SUCCESS;
	}
	if (!ok)
	{
		m_device.fn.destroy_shader_module(m_device.device(), p.vertex, nullptr);
		m_device.fn.destroy_shader_module(m_device.device(), p.fragment, nullptr);
		m_failed = true;
		return nullptr;
	}
	return &m_programs.emplace(key, p).first->second;
}

// The pipeline for one program and state, created on first use. The vertex input follows the frame's varyings, the
// viewport, scissor and blend constant are dynamic, and the colour and depth formats match begin_rendering().
VkPipeline retro_vk_engine::get_pipeline(const pipeline_key &key, const program &p)
{
	const auto found = m_pipelines.find(key);
	if (found != m_pipelines.end())
		return found->second;

	VkVertexInputBindingDescription binding = {};
	binding.stride = (5 + key.varyings) * 4;
	binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
	std::vector<VkVertexInputAttributeDescription> attributes;
	attributes.push_back({ 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0 });
	attributes.push_back({ 1, 0, VK_FORMAT_R32_UINT, 16 });
	for (uint32_t g = 0; g < (key.varyings + 3) / 4; g++)
	{
		const uint32_t components = std::min<uint32_t>(4, key.varyings - 4 * g);
		attributes.push_back({ 2 + g, 0, VARYING_FORMATS[components - 1], 20 + 16 * g });
	}
	VkPipelineVertexInputStateCreateInfo vertex_input = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
	vertex_input.vertexBindingDescriptionCount = 1;
	vertex_input.pVertexBindingDescriptions = &binding;
	vertex_input.vertexAttributeDescriptionCount = uint32_t(attributes.size());
	vertex_input.pVertexAttributeDescriptions = attributes.data();

	VkPipelineInputAssemblyStateCreateInfo input_assembly = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
	input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

	VkPipelineViewportStateCreateInfo viewport = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
	viewport.viewportCount = 1;
	viewport.scissorCount = 1;

	// both faces of every triangle are drawn
	VkPipelineRasterizationStateCreateInfo raster = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
	raster.polygonMode = VK_POLYGON_MODE_FILL;
	raster.cullMode = VK_CULL_MODE_NONE;
	raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	raster.lineWidth = 1.0f;

	// the sample count matches the colour and depth attachments of the target, and the rasteriser writes each sample
	VkPipelineMultisampleStateCreateInfo multisample = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
	multisample.rasterizationSamples = VkSampleCountFlagBits(key.samples);
	// with variable locations the draws set them (dynamic state), so the pipeline only enables them
	VkPipelineSampleLocationsStateCreateInfoEXT locations = { VK_STRUCTURE_TYPE_PIPELINE_SAMPLE_LOCATIONS_STATE_CREATE_INFO_EXT };
	const bool dynamic_locations = centre_samples(int(key.samples));
	if (dynamic_locations)
	{
		locations.sampleLocationsEnable = VK_TRUE;
		locations.sampleLocationsInfo.sType = VK_STRUCTURE_TYPE_SAMPLE_LOCATIONS_INFO_EXT;
		locations.sampleLocationsInfo.sampleLocationsPerPixel = VkSampleCountFlagBits(key.samples);
		locations.sampleLocationsInfo.sampleLocationGridSize.width = 1;
		locations.sampleLocationsInfo.sampleLocationGridSize.height = 1;
		locations.sampleLocationsInfo.sampleLocationsCount = key.samples;
		locations.sampleLocationsInfo.pSampleLocations = STANDARD_LOCATIONS_8;
		multisample.pNext = &locations;
	}

	VkPipelineDepthStencilStateCreateInfo depth = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
	depth.depthTestEnable = key.depth_test ? VK_TRUE : VK_FALSE;
	depth.depthWriteEnable = key.depth_write ? VK_TRUE : VK_FALSE;
	depth.depthCompareOp = VkCompareOp(key.depth_compare);

	VkPipelineColorBlendAttachmentState attachment = {};
	attachment.blendEnable = key.blend ? VK_TRUE : VK_FALSE;
	attachment.srcColorBlendFactor = VkBlendFactor(key.src);
	attachment.dstColorBlendFactor = VkBlendFactor(key.dst);
	attachment.colorBlendOp = VkBlendOp(key.blend_op);
	attachment.srcAlphaBlendFactor = VkBlendFactor(key.src_alpha);
	attachment.dstAlphaBlendFactor = VkBlendFactor(key.dst_alpha);
	attachment.alphaBlendOp = VkBlendOp(key.blend_op);
	attachment.colorWriteMask = key.color_write
			? (VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT)
			: 0;
	VkPipelineColorBlendStateCreateInfo blend = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
	blend.attachmentCount = 1;
	blend.pAttachments = &attachment;

	const VkDynamicState dynamic_states[4] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
			VK_DYNAMIC_STATE_SAMPLE_LOCATIONS_EXT };
	VkPipelineDynamicStateCreateInfo dynamic = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
	dynamic.dynamicStateCount = dynamic_locations ? 4 : 3;
	dynamic.pDynamicStates = dynamic_states;

	const VkFormat color_format = VK_FORMAT_B8G8R8A8_UNORM;
	VkPipelineRenderingCreateInfo rendering = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
	rendering.colorAttachmentCount = 1;
	rendering.pColorAttachmentFormats = &color_format;
	rendering.depthAttachmentFormat = key.depth_attachment ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_UNDEFINED;

	VkPipelineShaderStageCreateInfo stages[2] = {};
	stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = p.vertex;
	stages[0].pName = "main";
	stages[1] = stages[0];
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = p.fragment;

	VkGraphicsPipelineCreateInfo info = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
	info.pNext = &rendering;
	info.stageCount = 2;
	info.pStages = stages;
	info.pVertexInputState = &vertex_input;
	info.pInputAssemblyState = &input_assembly;
	info.pViewportState = &viewport;
	info.pRasterizationState = &raster;
	info.pMultisampleState = &multisample;
	info.pDepthStencilState = &depth;
	info.pColorBlendState = &blend;
	info.pDynamicState = &dynamic;
	info.layout = m_pipeline_layout;
	VkPipeline pipeline = VK_NULL_HANDLE;
	if (m_device.fn.create_graphics_pipelines(m_device.device(), m_pipeline_cache, 1, &info, nullptr, &pipeline) != VK_SUCCESS)
	{
		log_problem("pipeline", "graphics pipeline creation failed");
		m_failed = true;
		return VK_NULL_HANDLE;
	}
	m_pipelines.emplace(key, pipeline);
	m_pipeline_cache_dirty = true;
	return pipeline;
}

// Resolves every draw's program and pipeline before any command is recorded, so a failure leaves the frame unrecorded.
// The depth mode of the frame sets the depth test: ORDER always uses LESS and writes, as the OpenGL engine does.
bool retro_vk_engine::prepare_draws(const osd::gpu_frame &frame, bool use_depth, std::vector<VkPipeline> &pipelines)
{
	for (int i = 0; i < frame.command_count; i++)
	{
		const osd::gpu_command &c = frame.commands[i];
		if (c.type != osd::gpu_command_type::DRAW || c.vertex_count == 0)
			continue;
		const program *p = get_program(c.module, c.variant);
		if (!p)
			return false;

		pipeline_key key;
		key.module = c.module;
		key.variant = c.variant;
		key.varyings = uint32_t(frame.varyings_per_vertex);
		key.samples = uint32_t(std::max(m_target.msaa_samples, 1));
		key.depth_attachment = use_depth;
		key.depth_compare = VK_COMPARE_OP_ALWAYS;
		switch (frame.target.depth)
		{
		case osd::gpu_depth_mode::NONE:
			break;
		case osd::gpu_depth_mode::ORDER:
			key.depth_test = true;
			key.depth_compare = VK_COMPARE_OP_LESS;
			key.depth_write = true;
			break;
		case osd::gpu_depth_mode::REAL:
			key.depth_test = true;
			key.depth_compare = depth_func_vk(c.depth_func);
			key.depth_write = c.depth_write;
			break;
		}
		key.color_write = c.color_write;
		key.blend = c.blend.enable;
		key.blend_op = blend_op_vk(c.blend.op);
		key.src = blend_factor_vk(c.blend.src);
		key.dst = blend_factor_vk(c.blend.dst);
		key.src_alpha = blend_factor_vk(c.blend.separate_alpha ? c.blend.src_alpha : c.blend.src);
		key.dst_alpha = blend_factor_vk(c.blend.separate_alpha ? c.blend.dst_alpha : c.blend.dst);

		pipelines[i] = get_pipeline(key, *p);
		if (pipelines[i] == VK_NULL_HANDLE)
			return false;
	}
	return true;
}

// The descriptor set for one draw: the state buffer at binding 0 and the module's textures from binding 1. The set of
// the previous draw is reused when it names the same module and textures in this frame.
// A descriptor set with the module layout, from the frame's pools; a new pool is added when the last one fills.
bool retro_vk_engine::allocate_frame_set(VkDescriptorSet &set)
{
	const VkDevice dev = m_device.device();
	VkDescriptorSetAllocateInfo alloc = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
	alloc.descriptorSetCount = 1;
	alloc.pSetLayouts = &m_set_layout;
	for (;;)
	{
		const bool fresh = m_desc_pool_next == m_desc_pools.size();
		if (fresh)
		{
			VkDescriptorPoolSize sizes[2] = {};
			sizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
			sizes[0].descriptorCount = DESCRIPTOR_SETS_PER_POOL;
			sizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			sizes[1].descriptorCount = DESCRIPTOR_SETS_PER_POOL * osd::GPU_MAX_BINDINGS;
			VkDescriptorPoolCreateInfo pool_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
			pool_info.maxSets = DESCRIPTOR_SETS_PER_POOL;
			pool_info.poolSizeCount = 2;
			pool_info.pPoolSizes = sizes;
			VkDescriptorPool pool = VK_NULL_HANDLE;
			if (m_device.fn.create_descriptor_pool(dev, &pool_info, nullptr, &pool) != VK_SUCCESS)
				return false;
			m_desc_pools.push_back(pool);
		}
		alloc.descriptorPool = m_desc_pools[m_desc_pool_next];
		if (m_device.fn.allocate_descriptor_sets(dev, &alloc, &set) == VK_SUCCESS)
			break;
		if (fresh)
			return false;
		m_desc_pool_next++;
	}

	return true;
}

bool retro_vk_engine::allocate_set(const osd::gpu_command &c, const module &m, const staging_buffer &staging, size_t state_at, size_t state_bytes, VkDescriptorSet &set)
{
	const VkDevice dev = m_device.device();
	bool same = m_last_set != VK_NULL_HANDLE && m_last_module == c.module;
	for (int b = 0; same && b < osd::GPU_MAX_BINDINGS; b++)
		same = m_last_bindings[b] == c.bindings[b];
	if (same)
	{
		set = m_last_set;
		return true;
	}

	if (!allocate_frame_set(set))
		return false;

	VkDescriptorBufferInfo buffer = { staging.buffer, state_at, state_bytes };
	VkDescriptorImageInfo images[osd::GPU_MAX_BINDINGS] = {};
	VkWriteDescriptorSet writes[1 + osd::GPU_MAX_BINDINGS] = {};
	writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[0].dstSet = set;
	writes[0].dstBinding = 0;
	writes[0].descriptorCount = 1;
	writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	writes[0].pBufferInfo = &buffer;
	for (int b = 0; b < m.binding_count; b++)
	{
		const texture &t = m_textures.find(c.bindings[b])->second;
		images[b].sampler = sampler_for(t);
		images[b].imageView = t.img.view;
		images[b].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
		VkWriteDescriptorSet &w = writes[1 + b];
		w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		w.dstSet = set;
		w.dstBinding = uint32_t(1 + b);
		w.descriptorCount = 1;
		w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		w.pImageInfo = &images[b];
	}
	m_device.fn.update_descriptor_sets(dev, uint32_t(1 + m.binding_count), writes, 0, nullptr);

	m_last_set = set;
	m_last_module = c.module;
	std::copy(c.bindings, c.bindings + osd::GPU_MAX_BINDINGS, m_last_bindings);
	return true;
}

// The sampler of a texture: linear only for RGBA8 textures that ask for it, nearest for everything else.
VkSampler retro_vk_engine::sampler_for(const texture &t) const
{
	return (t.desc.format == osd::gpu_texture_format::RGBA8 && t.desc.filter_linear) ? m_sampler_linear : m_sampler_nearest;
}

// Opens the render pass for the frame's draws: the colour target and, when the frame has one, the depth target. The
// attachments load what is there, since the clears are recorded before the first draw.
void retro_vk_engine::begin_rendering(bool use_depth)
{
	VkRenderingAttachmentInfo color = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
	color.imageView = m_color.view;
	color.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
	color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
	color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	VkRenderingAttachmentInfo depth = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
	depth.imageView = m_depth.view;
	depth.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
	depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
	depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

	VkRenderingInfo info = { VK_STRUCTURE_TYPE_RENDERING_INFO };
	info.renderArea.extent.width = uint32_t(m_target.width);
	info.renderArea.extent.height = uint32_t(m_target.height);
	info.layerCount = 1;
	info.colorAttachmentCount = 1;
	info.pColorAttachments = &color;
	info.pDepthAttachment = use_depth ? &depth : nullptr;
	m_device.fn.cmd_begin_rendering(m_cmd, &info);
	m_rendering = true;
}

// Closes the render pass, if one is open. The barrier orders its writes before the next transfer or read-back.
void retro_vk_engine::end_rendering()
{
	if (!m_rendering)
		return;
	m_device.fn.cmd_end_rendering(m_cmd);
	m_rendering = false;
	record_barrier();
}

// Records one DRAW into the open render pass: the pipeline, the descriptor set, the dynamic state and the push constants,
// then the triangles, read from the frame's staging buffer.
bool retro_vk_engine::record_draw(const osd::gpu_command &c, VkPipeline pipeline, const osd::gpu_frame &frame, const staging_buffer &staging,
		size_t vertex_at, size_t state_at, size_t state_bytes)
{
	const module &m = m_modules.find(c.module)->second;
	VkDescriptorSet set = VK_NULL_HANDLE;
	if (!allocate_set(c, m, staging, state_at, state_bytes, set))
		return false;

	VkViewport viewport = { 0.0f, 0.0f, float(m_target.width), float(m_target.height), 0.0f, 1.0f };
	m_device.fn.cmd_set_viewport(m_cmd, 0, 1, &viewport);
	VkRect2D scissor = {};
	scissor.offset.x = m_clip[0];
	scissor.offset.y = m_clip[1];
	scissor.extent.width = uint32_t(m_clip[2]);
	scissor.extent.height = uint32_t(m_clip[3]);
	m_device.fn.cmd_set_scissor(m_cmd, 0, 1, &scissor);
	const float blend_constants[4] = { 0.0f, 0.0f, 0.0f, c.blend.constant_alpha };
	m_device.fn.cmd_set_blend_constants(m_cmd, blend_constants);

	m_device.fn.cmd_bind_pipeline(m_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
	m_device.fn.cmd_bind_descriptor_sets(m_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline_layout, 0, 1, &set, 0, nullptr);
	if (m_target.msaa_samples > 0 && centre_samples(m_target.msaa_samples))
		set_sample_locations(c.multisample);
	const push_constants push = {
		{ float(m_target.width), float(m_target.height) },
		1.0f / float(frame.prim_count + 1),
		frame.target.depth == osd::gpu_depth_mode::ORDER ? 1 : 0,
		std::max(1, (frame.state_stride + 3) / 4),
	};
	m_device.fn.cmd_push_constants(m_cmd, m_pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);

	const VkDeviceSize offset = vertex_at;
	m_device.fn.cmd_bind_vertex_buffers(m_cmd, 0, 1, &staging.buffer, &offset);
	m_device.fn.cmd_draw(m_cmd, c.vertex_count, 1, c.first_vertex, 0);
	m_stats.draw_calls++;
	return true;
}

// Forgets read-backs that were never collected, and the staging buffer they were read from. Nothing may be in flight.
void retro_vk_engine::drop_pending()
{
	m_pending.clear();
	destroy_staging(m_staging);
}

// Waits for the frame in flight and writes the pixels of its deferred read-backs.
bool retro_vk_engine::collect_pending()
{
	const auto start = clock_type::now();
	const bool ok = wait_in_flight();
	if (ok)
	{
		for (const pending_readback &p : m_pending)
			unpack_readback(p.rect, m_staging.data + p.at + readback_lut_bytes(p.rect));
	}
	drop_pending();
	m_stats.collect_ms = elapsed_ms(start);
	return ok;
}

bool retro_vk_engine::collect_frame()
{
	if (m_pending.empty())
		return true;
	return note_result(collect_pending(), m_collect_failures, "collect");
}

// Records one frame: uploads, the command list, then the trailing read-backs. A frame with read-backs is waited for
// unless it defers them, and a frame with a READBACK command in its list always is. Any other frame stays in flight.
bool retro_vk_engine::submit_pending(const osd::gpu_frame &frame)
{
	m_stats = osd::gpu_engine_stats();
	m_last_problem.clear();

	if (!validate_frame(frame))
		return false;

	// The previous frame must finish before its command buffer, descriptor pools and staging are reused. Its
	// uncollected read-backs are dropped, as the OpenGL engine drops them.
	if (!wait_in_flight())
		return false;
	drop_pending();
	if (m_gpu_ms_ready)
	{
		m_stats.gpu_ms = m_last_gpu_ms;
		m_gpu_ms_ready = false;
	}

	const auto upload_start = clock_type::now();
	osd::gpu_target_desc target = frame.target;
	fit_msaa(target);
	bool clear_color = false;
	bool clear_depth = false;
	if (!ensure_target(target, clear_color, clear_depth))
		return false;

	// nothing of the previous frame is in flight, so its descriptor sets can all be freed
	for (VkDescriptorPool pool : m_desc_pools)
		m_device.fn.reset_descriptor_pool(m_device.device(), pool, 0);
	m_desc_pool_next = 0;
	m_last_set = VK_NULL_HANDLE;
	m_last_module = osd::GPU_NULL_HANDLE;
	m_clip[0] = 0;
	m_clip[1] = 0;
	m_clip[2] = target.width;
	m_clip[3] = target.height;

	const bool use_depth = target.depth != osd::gpu_depth_mode::NONE;
	std::vector<VkPipeline> pipelines(frame.command_count, VK_NULL_HANDLE);
	if (!prepare_draws(frame, use_depth, pipelines))
		return false;

	// Offsets of every upload, read-back, vertex and state record in one staging buffer, in the order they are used.
	size_t total = 0;
	auto take = [&total](size_t bytes, size_t alignment = 16)
	{
		const size_t at = align_up(total, alignment);
		total = at + bytes;
		return at;
	};
	auto update_bytes = [this](const osd::gpu_texture_update &u)
	{
		return size_t(u.w) * u.h * texel_bytes(u.texture);
	};

	std::vector<size_t> update_at(frame.update_count);
	for (int i = 0; i < frame.update_count; i++)
	{
		update_at[i] = take(update_bytes(frame.updates[i]));
		m_stats.uploaded_bytes += update_bytes(frame.updates[i]);
	}
	std::vector<size_t> command_at(frame.command_count, 0);
	bool mid_list_readback = false;
	bool output_pass = false;
	for (int i = 0; i < frame.command_count; i++)
	{
		const osd::gpu_command &c = frame.commands[i];
		if (c.type == osd::gpu_command_type::UPDATE_TEXTURE)
		{
			command_at[i] = take(update_bytes(c.update));
			m_stats.uploaded_bytes += update_bytes(c.update);
		}
		else if (c.type == osd::gpu_command_type::READBACK)
		{
			command_at[i] = take(readback_bytes(c.readback));
			mid_list_readback = true;
			output_pass |= !c.readback.plain();
		}
	}
	std::vector<size_t> readback_at(frame.readback_count);
	for (int i = 0; i < frame.readback_count; i++)
	{
		readback_at[i] = take(readback_bytes(frame.readbacks[i]));
		output_pass |= !frame.readbacks[i].plain();
	}

	// the vertices as the frame gives them, then the state records padded to whole texels; the descriptor reads the
	// records at a 256-byte aligned offset, which every implementation accepts
	const size_t vertex_bytes = size_t(frame.vertex_count) * (size_t(5 + frame.varyings_per_vertex) * 4);
	const size_t vertex_at = take(vertex_bytes);
	const int rec_texels = std::max(1, (frame.state_stride + 3) / 4);
	const size_t state_bytes = std::max<size_t>(1, frame.prim_count) * rec_texels * 16;
	const size_t state_at = take(state_bytes, 256);

	// the read-back pass binds the whole staging buffer as one storage buffer
	if (output_pass && total > m_device.properties().limits.maxStorageBufferRange)
	{
		log_problem("frame", "staging buffer larger than the storage buffer limit for the read-back pass");
		return false;
	}
	if (total > 0 && !create_staging(total, m_staging))
	{
		log_problem("frame", "staging buffer allocation failed");
		return false;
	}
	for (int i = 0; i < frame.update_count; i++)
		write_update(frame.updates[i], m_staging.data + update_at[i]);
	for (int i = 0; i < frame.command_count; i++)
	{
		const osd::gpu_command &c = frame.commands[i];
		if (c.type == osd::gpu_command_type::UPDATE_TEXTURE)
			write_update(c.update, m_staging.data + command_at[i]);
		else if (c.type == osd::gpu_command_type::READBACK)
			write_readback_lut(c.readback, m_staging.data + command_at[i]);
	}
	for (int i = 0; i < frame.readback_count; i++)
		write_readback_lut(frame.readbacks[i], m_staging.data + readback_at[i]);
	if (vertex_bytes > 0)
		std::memcpy(m_staging.data + vertex_at, frame.vertices, vertex_bytes);
	if (target.msaa_samples > 0 && !centre_samples(target.msaa_samples))
		snap_triangles(frame, m_staging.data + vertex_at);
	std::memset(m_staging.data + state_at, 0, state_bytes);
	if (frame.prim_state && frame.prim_count > 0 && frame.state_stride > 0)
	{
		for (uint32_t i = 0; i < frame.prim_count; i++)
			std::memcpy(m_staging.data + state_at + size_t(i) * rec_texels * 16,
					frame.prim_state + size_t(i) * frame.state_stride, size_t(frame.state_stride) * 4);
	}
	m_stats.uploaded_bytes += vertex_bytes + state_bytes;
	m_stats.upload_ms = elapsed_ms(upload_start);

	const bool waits = mid_list_readback || (frame.readback_count > 0 && !frame.defer_readback);
	const auto draw_start = clock_type::now();
	if (!begin_recording())
		return false;
	m_rendering = false;
	if (m_timers)
	{
		m_device.fn.cmd_reset_query_pool(m_cmd, m_query_pool, 0, 2);
		m_device.fn.cmd_write_timestamp(m_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, m_query_pool, 0);
	}
	if (clear_color || clear_depth)
		record_clear(target.clear_color, clear_color, clear_depth);
	record_barrier();

	for (int i = 0; i < frame.update_count; i++)
	{
		record_update(frame.updates[i], update_at[i], m_staging);
		record_barrier();
	}
	for (int i = 0; i < frame.command_count; i++)
	{
		const osd::gpu_command &c = frame.commands[i];
		// draws and clip changes stay inside the render pass; every other command ends it first
		if (c.type == osd::gpu_command_type::DRAW)
		{
			if (c.vertex_count == 0)
				continue;
			if (!m_rendering)
				begin_rendering(use_depth);
			if (!record_draw(c, pipelines[i], frame, m_staging, vertex_at, state_at, state_bytes))
			{
				m_rendering = false;
				return false;
			}
			continue;
		}
		if (c.type == osd::gpu_command_type::SET_CLIP)
		{
			if (c.w <= 0 || c.h <= 0)
			{
				m_clip[0] = 0;
				m_clip[1] = 0;
				m_clip[2] = 0;
				m_clip[3] = 0;
			}
			else
			{
				const int x1 = std::clamp(c.x, 0, target.width);
				const int y1 = std::clamp(c.y, 0, target.height);
				const int x2 = std::clamp(c.x + c.w, 0, target.width);
				const int y2 = std::clamp(c.y + c.h, 0, target.height);
				m_clip[0] = x1;
				m_clip[1] = y1;
				m_clip[2] = x2 - x1;
				m_clip[3] = y2 - y1;
			}
			continue;
		}

		end_rendering();
		switch (c.type)
		{
		case osd::gpu_command_type::COPY_RECT:
			record_copy(c);
			break;
		case osd::gpu_command_type::UPDATE_TEXTURE:
			record_update(c.update, command_at[i], m_staging);
			break;
		case osd::gpu_command_type::READBACK:
			if (!record_readback_any(c.readback, command_at[i], m_staging))
				return false;
			break;
		case osd::gpu_command_type::CLEAR:
			record_clear(c.clear_color, c.clear_color_enable, c.clear_depth_enable);
			break;
		case osd::gpu_command_type::DRAW:
		case osd::gpu_command_type::SET_CLIP:
			break;
		}
		record_barrier();
	}
	end_rendering();

	const auto readback_start = clock_type::now();
	for (int i = 0; i < frame.readback_count; i++)
	{
		if (!record_readback_any(frame.readbacks[i], readback_at[i], m_staging))
			return false;
	}
	const double readback_record_ms = frame.readback_count > 0 ? elapsed_ms(readback_start) : 0.0;
	if (m_timers)
		m_device.fn.cmd_write_timestamp(m_cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_query_pool, 1);
	m_query_pending = m_timers;

	const bool ok = end_recording(waits);
	m_stats.draw_ms = elapsed_ms(draw_start);
	if (!ok)
		return false;
	if (m_gpu_ms_ready)
	{
		m_stats.gpu_ms = m_last_gpu_ms;
		m_gpu_ms_ready = false;
	}

	if (frame.readback_count > 0 && !waits)
	{
		// deferred: the pixels are written by collect_frame(), from the staging buffer that stays alive until then
		m_pending.clear();
		for (int i = 0; i < frame.readback_count; i++)
			m_pending.push_back({ frame.readbacks[i], readback_at[i] });
		m_stats.readback_ms = readback_record_ms;
		return true;
	}
	if (!waits)
		return true;

	// the read-back time is zero for a frame that reads nothing back
	const auto read_start = clock_type::now();
	for (int i = 0; i < frame.command_count; i++)
		if (frame.commands[i].type == osd::gpu_command_type::READBACK)
			unpack_readback(frame.commands[i].readback, m_staging.data + command_at[i] + readback_lut_bytes(frame.commands[i].readback));
	for (int i = 0; i < frame.readback_count; i++)
		unpack_readback(frame.readbacks[i], m_staging.data + readback_at[i] + readback_lut_bytes(frame.readbacks[i]));
	if (mid_list_readback || frame.readback_count > 0)
		m_stats.readback_ms = readback_record_ms + elapsed_ms(read_start);

	drop_pending();
	return true;
}
