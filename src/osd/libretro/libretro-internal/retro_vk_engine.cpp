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

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
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
size_t align16(size_t value)
{
	return (value + 15) & ~size_t(15);
}

} // anonymous namespace


retro_vk_engine::retro_vk_engine()
{
	std::string report;
	if (!m_device.create(report))
	{
		fprintf(stderr, "[retro_vk_engine] %s\n", report.c_str());
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
}

retro_vk_engine::~retro_vk_engine()
{
	if (!m_device.valid())
		return;
	for (auto &t : m_textures)
		destroy_image(t.second.img);
	m_textures.clear();
	release_target();
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
	img.width = width;
	img.height = height;
	return true;
}

void retro_vk_engine::destroy_image(image &img)
{
	const VkDevice dev = m_device.device();
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
	info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
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

// Refuses what this stage does not run yet, and the same frame errors the OpenGL engine refuses.
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

	for (int i = 0; i < frame.update_count; i++)
		if (!validate_update(frame.updates[i]))
			return false;

	for (int i = 0; i < frame.command_count; i++)
	{
		const osd::gpu_command &c = frame.commands[i];
		switch (c.type)
		{
		case osd::gpu_command_type::DRAW:
			log_problem("draw", "draw commands need the Vulkan pipelines (not built yet)");
			return false;
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

bool retro_vk_engine::submit_pending(const osd::gpu_frame &frame)
{
	m_stats = osd::gpu_engine_stats();

	if (!validate_frame(frame))
		return false;

	const auto upload_start = clock_type::now();
	osd::gpu_target_desc target = frame.target;
	target.msaa_samples = 0;
	bool clear_color = false;
	bool clear_depth = false;
	if (!ensure_target(target, clear_color, clear_depth))
		return false;

	// Offsets of every upload and read-back in one staging buffer, in the order they are used.
	size_t total = 0;
	auto take = [&total](size_t bytes)
	{
		const size_t at = align16(total);
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
	m_stats.upload_ms = elapsed_ms(upload_start);

	const auto draw_start = clock_type::now();
	if (!begin_recording())
	{
		destroy_staging(staging);
		return false;
	}
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
		switch (c.type)
		{
		case osd::gpu_command_type::DRAW:
			// refused by validate_frame() until the draw path exists
			break;
		case osd::gpu_command_type::SET_CLIP:
			// the clip is applied by DRAW, which this stage does not run
			break;
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
		}
		record_barrier();
	}
	for (int i = 0; i < frame.readback_count; i++)
		record_readback(frame.readbacks[i], readback_at[i], staging);

	const bool ok = end_recording();
	m_stats.draw_ms = elapsed_ms(draw_start);
	if (!ok)
	{
		destroy_staging(staging);
		return false;
	}

	const auto read_start = clock_type::now();
	for (int i = 0; i < frame.command_count; i++)
		if (frame.commands[i].type == osd::gpu_command_type::READBACK)
			unpack_readback(frame.commands[i].readback, staging.data + command_at[i]);
	for (int i = 0; i < frame.readback_count; i++)
		unpack_readback(frame.readbacks[i], staging.data + readback_at[i]);
	m_stats.readback_ms = elapsed_ms(read_start);

	destroy_staging(staging);
	return true;
}
