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
	m_caps.max_samples = 0;     // multisampled targets come with VK.5

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
}

retro_vk_engine::~retro_vk_engine()
{
	if (!m_device.valid())
		return;
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

bool retro_vk_engine::create_image(image &img, VkFormat format, int width, int height, VkImageUsageFlags usage, VkImageAspectFlags aspect)
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
	info.samples = VK_SAMPLE_COUNT_1_BIT;
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
	return end_recording();
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
	// nothing is in flight once submit_frame() has returned, so the image can go now
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
			|| target.depth != m_target.depth;

	clear_color = !target.persistent;
	clear_depth = target.depth == osd::gpu_depth_mode::ORDER || !target.persistent;

	if (realloc)
	{
		release_target();

		const VkImageUsageFlags color_usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		bool ok = create_image(m_color, VK_FORMAT_B8G8R8A8_UNORM, target.width, target.height, color_usage, VK_IMAGE_ASPECT_COLOR_BIT)
				&& create_image(m_scratch, VK_FORMAT_B8G8R8A8_UNORM, target.width, target.height, color_usage, VK_IMAGE_ASPECT_COLOR_BIT)
				&& transition_to_general(m_color)
				&& transition_to_general(m_scratch);
		if (ok && target.depth != osd::gpu_depth_mode::NONE)
		{
			const VkImageUsageFlags depth_usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
			ok = create_image(m_depth, VK_FORMAT_D32_SFLOAT, target.width, target.height, depth_usage, VK_IMAGE_ASPECT_DEPTH_BIT)
					&& transition_to_general(m_depth);
		}
		if (!ok)
		{
			release_target();
			log_problem("render target", "image allocation failed");
			return false;
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
	m_target_valid = false;
}


//**************************************************************************
//  Command recording
//**************************************************************************

// Records into m_cmd. The command pool is reset here, which is safe because every
// earlier submission has been waited for.
bool retro_vk_engine::begin_recording()
{
	if (m_device.fn.reset_command_pool(m_device.device(), m_device.command_pool(), 0) != VK_SUCCESS)
		return false;
	VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	return m_device.fn.begin_command_buffer(m_cmd, &begin) == VK_SUCCESS;
}

// Submits what was recorded and waits for it. The engine never leaves work in flight across calls.
bool retro_vk_engine::end_recording()
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
	return m_device.fn.wait_for_fences(dev, 1, &m_fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS;
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
	VkBufferImageCopy region = {};
	region.bufferOffset = offset;
	region.imageSubresource = layers(VK_IMAGE_ASPECT_COLOR_BIT);
	region.imageOffset.x = r.x;
	region.imageOffset.y = r.y;
	region.imageExtent.width = uint32_t(r.w);
	region.imageExtent.height = uint32_t(r.h);
	region.imageExtent.depth = 1;
	m_device.fn.cmd_copy_image_to_buffer(m_cmd, m_color.handle, VK_IMAGE_LAYOUT_GENERAL, staging.buffer, 1, &region);
}

void retro_vk_engine::unpack_readback(const osd::gpu_readback_rect &r, const uint8_t *src) const
{
	const size_t row_bytes = size_t(r.w) * 4;
	for (int row = 0; row < r.h; row++)
	{
		const uint8_t *line = src + size_t(row) * row_bytes;
		if (r.format == osd::gpu_readback_format::RG8)
		{
			// the red and green bytes of each BGRA pixel, as GL_RG / GL_UNSIGNED_BYTE gives them
			uint16_t *out = r.out16 + size_t(row) * r.stride;
			for (int col = 0; col < r.w; col++)
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

// Refuses the frames the OpenGL engine refuses, with the same checks.
bool retro_vk_engine::validate_frame(const osd::gpu_frame &frame)
{
	const osd::gpu_target_desc &tg = frame.target;
	if (tg.width <= 0 || tg.height <= 0 || tg.width > m_caps.max_texture_size || tg.height > m_caps.max_texture_size)
	{
		log_problem("frame", "target size out of range");
		return false;
	}
	// multisampled targets come with VK.5; the factory keeps those sessions on OpenGL
	if (tg.msaa_samples > 0)
	{
		log_problem("frame", "multisampled target, not supported by the Vulkan engine yet");
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
	const bool rg8 = r.format == osd::gpu_readback_format::RG8;
	if (!r.plain() || (rg8 ? !r.out16 : !r.out) || r.w <= 0 || r.h <= 0 || r.x < 0 || r.y < 0
			|| r.x + r.w > tg.width || r.y + r.h > tg.height || r.stride < r.w)
	{
		log_problem("readback", "rectangle outside the target, bad buffer, or not a plain read-back");
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

	// the targets are single-sample: frames are drawn without multisampling until VK.5
	VkPipelineMultisampleStateCreateInfo multisample = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

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

	const VkDynamicState dynamic_states[3] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS };
	VkPipelineDynamicStateCreateInfo dynamic = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
	dynamic.dynamicStateCount = 3;
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

bool retro_vk_engine::submit_pending(const osd::gpu_frame &frame)
{
	m_stats = osd::gpu_engine_stats();

	if (!validate_frame(frame))
		return false;

	const auto upload_start = clock_type::now();
	osd::gpu_target_desc target = frame.target;
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
	auto readback_bytes = [](const osd::gpu_readback_rect &r)
	{
		return size_t(r.w) * r.h * 4;
	};

	std::vector<size_t> update_at(frame.update_count);
	for (int i = 0; i < frame.update_count; i++)
	{
		update_at[i] = take(update_bytes(frame.updates[i]));
		m_stats.uploaded_bytes += update_bytes(frame.updates[i]);
	}
	std::vector<size_t> command_at(frame.command_count, 0);
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
		}
	}
	std::vector<size_t> readback_at(frame.readback_count);
	for (int i = 0; i < frame.readback_count; i++)
		readback_at[i] = take(readback_bytes(frame.readbacks[i]));

	// the vertices as the frame gives them, then the state records padded to whole texels; the descriptor reads the
	// records at a 256-byte aligned offset, which every implementation accepts
	const size_t vertex_bytes = size_t(frame.vertex_count) * (size_t(5 + frame.varyings_per_vertex) * 4);
	const size_t vertex_at = take(vertex_bytes);
	const int rec_texels = std::max(1, (frame.state_stride + 3) / 4);
	const size_t state_bytes = std::max<size_t>(1, frame.prim_count) * rec_texels * 16;
	const size_t state_at = take(state_bytes, 256);

	staging_buffer staging;
	if (total > 0 && !create_staging(total, staging))
	{
		log_problem("frame", "staging buffer allocation failed");
		return false;
	}
	for (int i = 0; i < frame.update_count; i++)
		write_update(frame.updates[i], staging.data + update_at[i]);
	for (int i = 0; i < frame.command_count; i++)
		if (frame.commands[i].type == osd::gpu_command_type::UPDATE_TEXTURE)
			write_update(frame.commands[i].update, staging.data + command_at[i]);
	if (vertex_bytes > 0)
		std::memcpy(staging.data + vertex_at, frame.vertices, vertex_bytes);
	std::memset(staging.data + state_at, 0, state_bytes);
	if (frame.prim_state && frame.prim_count > 0 && frame.state_stride > 0)
	{
		for (uint32_t i = 0; i < frame.prim_count; i++)
			std::memcpy(staging.data + state_at + size_t(i) * rec_texels * 16,
					frame.prim_state + size_t(i) * frame.state_stride, size_t(frame.state_stride) * 4);
	}
	m_stats.uploaded_bytes += vertex_bytes + state_bytes;
	m_stats.upload_ms = elapsed_ms(upload_start);

	const auto draw_start = clock_type::now();
	if (!begin_recording())
	{
		destroy_staging(staging);
		return false;
	}
	m_rendering = false;
	if (clear_color || clear_depth)
		record_clear(target.clear_color, clear_color, clear_depth);
	record_barrier();

	for (int i = 0; i < frame.update_count; i++)
	{
		record_update(frame.updates[i], update_at[i], staging);
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
			if (!record_draw(c, pipelines[i], frame, staging, vertex_at, state_at, state_bytes))
			{
				m_rendering = false;
				destroy_staging(staging);
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
			record_update(c.update, command_at[i], staging);
			break;
		case osd::gpu_command_type::READBACK:
			record_readback(c.readback, command_at[i], staging);
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
	for (int i = 0; i < frame.readback_count; i++)
		record_readback(frame.readbacks[i], readback_at[i], staging);

	const bool ok = end_recording();
	m_stats.draw_ms = elapsed_ms(draw_start);
	if (!ok)
	{
		destroy_staging(staging);
		return false;
	}

	// the read-back time is zero for a frame that reads nothing back
	const bool reads_back = frame.readback_count > 0 || std::any_of(frame.commands, frame.commands + frame.command_count,
			[](const osd::gpu_command &c) { return c.type == osd::gpu_command_type::READBACK; });
	const auto read_start = clock_type::now();
	for (int i = 0; i < frame.command_count; i++)
		if (frame.commands[i].type == osd::gpu_command_type::READBACK)
			unpack_readback(frame.commands[i].readback, staging.data + command_at[i]);
	for (int i = 0; i < frame.readback_count; i++)
		unpack_readback(frame.readbacks[i], staging.data + readback_at[i]);
	if (reads_back)
		m_stats.readback_ms = elapsed_ms(read_start);

	destroy_staging(staging);
	return true;
}
