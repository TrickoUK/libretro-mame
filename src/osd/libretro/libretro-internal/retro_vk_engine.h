// license:BSD-3-Clause
// copyright-holders:TrickoUK
/***************************************************************************
    retro_vk_engine.h
    Vulkan implementation of osd::gpu_engine on the private device from
    retro_vk_device. It owns the textures, the render target and its scratch
    copy, and runs UPDATE_TEXTURE, CLEAR, COPY_RECT, SET_CLIP, DRAW and
    read-back. A read-back that is not plain (downsample, opaque, coverage,
    priority bits, channel lookup) runs a compute pass that writes the output
    pixels into the frame's staging buffer. Each module's shader pair is
    compiled per variant, and its pipelines are created on first use and kept
    in a VkPipelineCache under the system's gpu-cache directory. Each frame is
    recorded into one command buffer. A frame with read-backs (and without
    defer_readback) is waited for before submit_frame() returns; any other
    frame returns once it is submitted, and the next call waits for it first.
    Only compiled when HAVE_RETRO_GPU_TARGET is defined.
***************************************************************************/

#ifndef MAME_OSD_LIBRETRO_RETRO_VK_ENGINE_H
#define MAME_OSD_LIBRETRO_RETRO_VK_ENGINE_H

#pragma once

#include "retro_vk_device.h"
#include "interface/gpuengine.h"

#include <cstdint>
#include <map>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>


class retro_vk_engine : public osd::gpu_engine
{
public:
	// the device is owned by the caller (the OSD's engine factory) and outlives the engine
	explicit retro_vk_engine(retro_vk::vk_device &device);
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
	virtual bool collect_frame() override;
	virtual const osd::gpu_engine_stats &last_frame_stats() const override { return m_stats; }

private:
	// an image in GENERAL layout, the layout every image stays in
	struct image
	{
		VkImage handle = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
		VkFormat format = VK_FORMAT_UNDEFINED;
		VkImageView view = VK_NULL_HANDLE;
		int width = 0;
		int height = 0;
	};

	struct texture
	{
		osd::gpu_texture_desc desc;
		image img;
		uint64_t bytes = 0;
	};

	// a read-back waiting for collect_frame(): its rectangle, and where its bytes are in the staging buffer
	struct pending_readback
	{
		osd::gpu_readback_rect rect;
		size_t at = 0;
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

	// one host-visible buffer per frame: uploads first, then read-backs, the vertices and the state records
	struct staging_buffer
	{
		VkBuffer buffer = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		uint8_t *data = nullptr;
	};

	// the two shader stages of one module and variant
	struct program
	{
		VkShaderModule vertex = VK_NULL_HANDLE;
		VkShaderModule fragment = VK_NULL_HANDLE;
	};

	// everything a graphics pipeline depends on apart from the program itself
	struct pipeline_key
	{
		osd::gpu_handle module = osd::GPU_NULL_HANDLE;
		uint32_t variant = 0;
		uint32_t varyings = 0;
		uint32_t samples = 1;           // the target's sample count: the pipeline's rasterisation samples
		bool depth_attachment = false;
		bool depth_test = false;
		uint32_t depth_compare = 0;     // VkCompareOp
		bool depth_write = false;
		bool color_write = false;
		bool blend = false;
		uint32_t blend_op = 0;          // VkBlendOp
		uint32_t src = 0, dst = 0;      // VkBlendFactor
		uint32_t src_alpha = 0, dst_alpha = 0;

		bool operator<(const pipeline_key &o) const
		{
			return std::tie(module, variant, varyings, samples, depth_attachment, depth_test, depth_compare, depth_write, color_write,
					blend, blend_op, src, dst, src_alpha, dst_alpha)
					< std::tie(o.module, o.variant, o.varyings, o.samples, o.depth_attachment, o.depth_test, o.depth_compare,
					o.depth_write, o.color_write, o.blend, o.blend_op, o.src, o.dst, o.src_alpha, o.dst_alpha);
		}
	};

	void log_problem(const char *what, const char *detail);
	bool note_result(bool ok, int &streak, const char *what);

	bool validate_frame(const osd::gpu_frame &frame);
	void fit_msaa(osd::gpu_target_desc &target);
	bool sample_count_supported(int samples, bool sampled) const;
	const image &readback_image() const { return m_target.msaa_samples > 0 ? m_resolve : m_color; }
	// whether draws on a target with this many samples place their samples by VK_EXT_sample_locations: every sample at
	// the pixel centre for a draw without multisampling, the standard pattern otherwise
	bool centre_samples(int samples) const
	{
		return samples > 1 && m_device.variable_sample_locations() && (m_device.sample_location_counts() & uint32_t(samples)) != 0;
	}
	void set_sample_locations(bool multisample);
	void snap_triangles(const osd::gpu_frame &frame, uint8_t *vertices) const;
	bool reads_samples(const osd::gpu_readback_rect &r) const;
	bool validate_update(const osd::gpu_texture_update &u);
	bool valid_readback(const osd::gpu_readback_rect &r, const osd::gpu_target_desc &tg);
	bool submit_pending(const osd::gpu_frame &frame);
	bool collect_pending();
	bool wait_in_flight();
	void read_timer();
	void drop_pending();
	bool ensure_output_pipeline(bool per_sample);
	bool allocate_frame_set(VkDescriptorSet &set);
	bool record_output_pass(const osd::gpu_readback_rect &r, size_t at, const staging_buffer &staging);
	size_t readback_lut_bytes(const osd::gpu_readback_rect &r) const;
	size_t readback_bytes(const osd::gpu_readback_rect &r) const;
	void write_readback_lut(const osd::gpu_readback_rect &r, uint8_t *dst) const;
	std::string device_key() const;

	bool create_pipeline_objects();
	void destroy_pipeline_objects();
	void load_pipeline_cache();
	void save_pipeline_cache();
	const program *get_program(osd::gpu_handle module_handle, uint32_t variant);
	VkPipeline get_pipeline(const pipeline_key &key, const program &p);
	bool prepare_draws(const osd::gpu_frame &frame, bool use_depth, std::vector<VkPipeline> &pipelines);
	bool allocate_set(const osd::gpu_command &c, const module &m, const staging_buffer &staging, size_t state_at, size_t state_bytes, VkDescriptorSet &set);
	void begin_rendering(bool use_depth);
	void end_rendering();
	bool record_draw(const osd::gpu_command &c, VkPipeline pipeline, const osd::gpu_frame &frame, const staging_buffer &staging,
			size_t vertex_at, size_t state_at, size_t state_bytes);

	int texel_bytes(osd::gpu_handle texture) const;
	VkSampler sampler_for(const texture &t) const;
	bool create_image(image &img, VkFormat format, int width, int height, VkImageUsageFlags usage, VkImageAspectFlags aspect, int samples = 1,
			VkImageCreateFlags flags = 0);
	void destroy_image(image &img);
	bool transition_to_general(const image &img);
	bool ensure_target(const osd::gpu_target_desc &target, bool &clear_color, bool &clear_depth);
	void release_target();

	bool begin_recording();
	bool end_recording(bool wait);
	void record_barrier();
	void record_clear(const float *rgba, bool color, bool depth);
	void record_clear_multisample(const float *rgba, bool color, bool depth);
	void record_resolve();
	void record_copy(const osd::gpu_command &c);
	void record_update(const osd::gpu_texture_update &u, size_t offset, const staging_buffer &staging);
	void record_readback(const osd::gpu_readback_rect &r, size_t offset, const staging_buffer &staging);
	bool record_readback_any(const osd::gpu_readback_rect &r, size_t at, const staging_buffer &staging);

	bool create_staging(size_t bytes, staging_buffer &staging);
	void destroy_staging(staging_buffer &staging);
	void write_update(const osd::gpu_texture_update &u, uint8_t *dst) const;
	void unpack_readback(const osd::gpu_readback_rect &r, const uint8_t *src) const;

	static constexpr int MAX_FAILED_FRAMES = 30;

	retro_vk::vk_device &m_device;
	VkCommandPool m_pool = VK_NULL_HANDLE;
	VkCommandBuffer m_cmd = VK_NULL_HANDLE;
	VkFence m_fence = VK_NULL_HANDLE;

	// shared pipeline state: one set layout (binding 0 the state buffer, bindings 1..8 the samplers), one pipeline
	// layout with the per-frame push constants, the samplers and the pipeline cache
	VkDescriptorSetLayout m_set_layout = VK_NULL_HANDLE;
	VkPipelineLayout m_pipeline_layout = VK_NULL_HANDLE;
	VkSampler m_sampler_nearest = VK_NULL_HANDLE;
	VkSampler m_sampler_linear = VK_NULL_HANDLE;
	VkPipelineCache m_pipeline_cache = VK_NULL_HANDLE;
	// the read-back compute passes, [0] for a single-sample target (or one read once) and [1] for samples read one by one:
	// each its module and pipeline; the layout (and the set layout) is shared
	VkShaderModule m_output_module[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
	VkPipeline m_output_pipeline[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
	VkPipelineLayout m_output_layout = VK_NULL_HANDLE;
	bool m_pipeline_cache_dirty = false;
	std::string m_cache_dir;

	std::map<std::pair<osd::gpu_handle, uint32_t>, program> m_programs;
	std::map<pipeline_key, VkPipeline> m_pipelines;

	// descriptor pools are reset at the start of each frame, when nothing is in flight; a new one is added when one fills
	std::vector<VkDescriptorPool> m_desc_pools;
	size_t m_desc_pool_next = 0;
	VkDescriptorSet m_last_set = VK_NULL_HANDLE;
	// the frame in flight: its staging buffer stays alive until its read-backs are collected or the next frame starts
	bool m_in_flight = false;
	staging_buffer m_staging;
	std::vector<pending_readback> m_pending;
	int m_collect_failures = 0;
	// GPU timestamps around the command loop, only when MAME_GPU_PROF is set and the queue supports them
	bool m_timers = false;
	VkQueryPool m_query_pool = VK_NULL_HANDLE;
	double m_timestamp_period = 0.0;
	uint64_t m_timestamp_mask = 0;
	bool m_query_pending = false;
	bool m_gpu_ms_ready = false;
	double m_last_gpu_ms = 0.0;
	// the first refused frame's reason, logged once with the session's decision
	std::string m_last_problem;
	std::string m_first_refusal;
	osd::gpu_handle m_last_module = osd::GPU_NULL_HANDLE;
	osd::gpu_handle m_last_bindings[osd::GPU_MAX_BINDINGS] = {};

	// dynamic state the draws set: the scissor (SET_CLIP) and whether a render pass is open
	int m_clip[4] = { 0, 0, 0, 0 };
	bool m_rendering = false;

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
	// same-size copy used by COPY_RECT so overlapping rectangles copy from the original pixels. A multisampled
	// target has its colour, depth and copy at the sample count; m_resolve is the single-sample colour that
	// read-backs read, filled from m_color by record_resolve()
	osd::gpu_target_desc m_target;
	bool m_target_valid = false;
	image m_color;
	image m_depth;
	image m_scratch;
	image m_resolve;

	// multisampling: the sample counts the device supports for colour and depth targets and for sampled colour,
	// the largest count that is supported, and the memory budget the target's samples must fit in
	uint32_t m_sample_counts = 0;
	uint32_t m_sampled_sample_counts = 0;
	uint64_t m_msaa_budget = 1024ULL * 1024 * 1024;
	// the sample locations the command buffer was last given: 0 standard, 1 centre, -1 none yet
	int m_locations_mode = -1;
	int m_msaa_logged_requested = 0, m_msaa_logged_granted = 0;
};

#endif // MAME_OSD_LIBRETRO_RETRO_VK_ENGINE_H
