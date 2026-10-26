// license:BSD-3-Clause
// copyright-holders:TrickoUK
/***************************************************************************
    retro_vk_engine.h
    Vulkan implementation of osd::gpu_engine on the private device from
    retro_vk_device. This stage (VK.3b) owns the textures, the render
    target and its scratch copy, and runs UPDATE_TEXTURE, CLEAR, COPY_RECT,
    SET_CLIP and plain read-back. Draw commands and the pixel pipelines come
    with VK.3c, so a frame that draws is refused. Each frame is recorded into
    one command buffer and waited for before submit_frame() returns. The
    factory in retromain.cpp still returns the OpenGL engine.
    Only compiled when HAVE_RETRO_GPU_TARGET is defined.
***************************************************************************/

#ifndef MAME_OSD_LIBRETRO_RETRO_VK_ENGINE_H
#define MAME_OSD_LIBRETRO_RETRO_VK_ENGINE_H

#pragma once

#include "retro_vk_device.h"
#include "interface/gpuengine.h"

#include <cstdint>
#include <string>
#include <unordered_map>


class retro_vk_engine : public osd::gpu_engine
{
public:
	retro_vk_engine();
	virtual ~retro_vk_engine();

	retro_vk_engine(const retro_vk_engine &) = delete;
	retro_vk_engine &operator=(const retro_vk_engine &) = delete;

	// true once the device and the frame resources exist
	bool is_valid() const { return m_device.valid(); }

	virtual const osd::gpu_engine_caps &caps() const override { return m_caps; }
	virtual bool failed() const override { return !m_device.valid() || m_failed || m_device.failed(); }

	virtual osd::gpu_handle create_texture(const osd::gpu_texture_desc &desc) override;
	virtual void destroy_texture(osd::gpu_handle texture) override;
	virtual uint64_t texture_memory() const override { return m_texture_bytes; }
	virtual void set_texture_budget(uint64_t bytes) override { m_texture_budget = bytes; }

	virtual osd::gpu_handle create_shader_module(const osd::gpu_shader_module &module) override;
	virtual void destroy_shader_module(osd::gpu_handle module) override;

	virtual bool submit_frame(const osd::gpu_frame &frame) override;
	virtual const osd::gpu_engine_stats &last_frame_stats() const override { return m_stats; }

private:
	// an image in GENERAL layout, the layout every image stays in
	struct image
	{
		VkImage handle = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
		int width = 0;
		int height = 0;
	};

	struct texture
	{
		osd::gpu_texture_desc desc;
		image img;
		uint64_t bytes = 0;
	};

	struct module
	{
		std::string name;
		std::string source;
		uint32_t centroid_mask = 0;
		int varying_count = 0;
		osd::gpu_varying_mode varying_mode[osd::GPU_MAX_VARYINGS] = {};
		int state_stride = 0;
		bool writes_depth = false;
		bool blend_factor_output = false;
		int binding_count = 0;
		std::string binding_name[osd::GPU_MAX_BINDINGS];
		osd::gpu_texture_format binding_format[osd::GPU_MAX_BINDINGS] = {};
	};

	// one host-visible buffer per frame: uploads first, then read-backs
	struct staging_buffer
	{
		VkBuffer buffer = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		uint8_t *data = nullptr;
	};

	void log_problem(const char *what, const char *detail);
	bool note_result(bool ok, int &streak, const char *what);

	bool validate_frame(const osd::gpu_frame &frame);
	bool validate_update(const osd::gpu_texture_update &u);
	bool valid_readback(const osd::gpu_readback_rect &r, const osd::gpu_target_desc &tg);
	bool submit_pending(const osd::gpu_frame &frame);

	int texel_bytes(osd::gpu_handle texture) const;
	bool create_image(image &img, VkFormat format, int width, int height, VkImageUsageFlags usage, VkImageAspectFlags aspect);
	void destroy_image(image &img);
	bool transition_to_general(const image &img);
	bool ensure_target(const osd::gpu_target_desc &target, bool &clear_color, bool &clear_depth);
	void release_target();

	bool begin_recording();
	bool end_recording();
	void record_barrier();
	void record_clear(const float *rgba, bool color, bool depth);
	void record_copy(const osd::gpu_command &c);
	void record_update(const osd::gpu_texture_update &u, size_t offset, const staging_buffer &staging);
	void record_readback(const osd::gpu_readback_rect &r, size_t offset, const staging_buffer &staging);

	bool create_staging(size_t bytes, staging_buffer &staging);
	void destroy_staging(staging_buffer &staging);
	void write_update(const osd::gpu_texture_update &u, uint8_t *dst) const;
	void unpack_readback(const osd::gpu_readback_rect &r, const uint8_t *src) const;

	static constexpr int MAX_FAILED_FRAMES = 30;

	retro_vk::vk_device m_device;
	VkCommandBuffer m_cmd = VK_NULL_HANDLE;
	VkFence m_fence = VK_NULL_HANDLE;

	osd::gpu_engine_caps m_caps;
	std::string m_renderer;
	osd::gpu_engine_stats m_stats;
	bool m_failed = false;
	int m_submit_failures = 0;
	int m_problem_count = 0;

	osd::gpu_handle m_next_handle = 1;
	std::unordered_map<osd::gpu_handle, texture> m_textures;
	std::unordered_map<osd::gpu_handle, module> m_modules;
	uint64_t m_texture_bytes = 0;
	uint64_t m_texture_budget = 512ULL * 1024 * 1024;

	// render target: BGRA8 colour (the byte order a BGRA read-back produces), optional D32 depth, and a
	// same-size copy used by COPY_RECT so overlapping rectangles copy from the original pixels
	osd::gpu_target_desc m_target;
	bool m_target_valid = false;
	image m_color;
	image m_depth;
	image m_scratch;
};

#endif // MAME_OSD_LIBRETRO_RETRO_VK_ENGINE_H
