// license:BSD-3-Clause
// copyright-holders:TrickoUK
/***************************************************************************

    retro_gpu_engine.h

    Headless EGL/OpenGL implementation of osd::gpu_engine for the libretro
    OSD. It owns a private GL 3.3 core context on the surfaceless EGL
    platform, independent of the frontend's own, and makes it current only
    inside submit_frame(). The frontend can perform its own EGL work from
    inside a core call (for example when a resolution change reaches it as
    SET_SYSTEM_AV_INFO), and keeping a second context current across
    separately scheduled calls was found to corrupt driver state, so the
    context is taken and released within one synchronous call. See
    interface/gpuengine.h for the frame model.

    Only compiled when HAVE_RETRO_GPU_TARGET is defined.

***************************************************************************/
#ifndef MAME_OSD_LIBRETRO_RETRO_GPU_ENGINE_H
#define MAME_OSD_LIBRETRO_RETRO_GPU_ENGINE_H

#pragma once

#include "interface/gpuengine.h"

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

class retro_gpu_engine : public osd::gpu_engine
{
public:
	retro_gpu_engine();
	virtual ~retro_gpu_engine();

	retro_gpu_engine(const retro_gpu_engine &) = delete;
	retro_gpu_engine &operator=(const retro_gpu_engine &) = delete;

	// true once EGL/GL initialisation succeeded
	bool is_valid() const { return m_valid; }

	virtual const osd::gpu_engine_caps &caps() const override { return m_caps; }
	virtual bool failed() const override { return !m_valid || m_failed; }

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
	struct texture
	{
		osd::gpu_texture_desc desc;
		uint32_t gl_id = 0;             // created at first use inside a frame
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
		int binding_count = 0;
		std::string binding_name[osd::GPU_MAX_BINDINGS];
		osd::gpu_texture_format binding_format[osd::GPU_MAX_BINDINGS] = {};
	};

	struct program
	{
		uint32_t gl_id = 0;
		int u_target_size = -1;
		int u_order_scale = -1;
		int u_depth_order = -1;
		int u_rec_texels = -1;
	};

	bool init_context();
	void shutdown();
	bool validate_frame(const osd::gpu_frame &frame);
	bool validate_update(const osd::gpu_texture_update &u);
	bool apply_update(const osd::gpu_texture_update &u);
	bool ensure_texture(texture &t);
	void ensure_state_texture(const osd::gpu_frame &frame);
	void ensure_target(const osd::gpu_target_desc &target);
	void release_target();
	const program *get_program(osd::gpu_handle module_handle, uint32_t variant);
	void apply_blend(const osd::gpu_blend_state &blend);
	void flush_deleted();
	void log_problem(const char *what, const char *detail);

	void *m_display = nullptr;
	void *m_context = nullptr;
	void *m_surface = nullptr;
	bool m_valid = false;
	bool m_failed = false;

	osd::gpu_engine_caps m_caps;
	std::string m_renderer;
	osd::gpu_engine_stats m_stats;

	osd::gpu_handle m_next_handle = 1;
	std::unordered_map<osd::gpu_handle, texture> m_textures;
	std::unordered_map<osd::gpu_handle, module> m_modules;
	std::map<std::pair<osd::gpu_handle, uint32_t>, program> m_programs;
	std::vector<uint32_t> m_deleted_textures;       // GL ids waiting for a context
	std::vector<uint32_t> m_deleted_programs;
	uint64_t m_texture_bytes = 0;
	uint64_t m_texture_budget = 512ULL * 1024 * 1024;

	// per-primitive state table (RGBA32I, 2048 texels wide)
	uint32_t m_state_tex = 0;
	int m_state_tex_height = 0;

	// current render target
	osd::gpu_target_desc m_target;
	bool m_target_valid = false;
	uint32_t m_fbo = 0;
	uint32_t m_color_rb = 0;
	uint32_t m_color_tex = 0;           // the multisample colour when its samples must be readable, instead of m_color_rb
	uint32_t m_depth_rb = 0;
	uint32_t m_resolve_fbo = 0;
	uint32_t m_resolve_tex = 0;

	uint32_t m_vao = 0;
	uint32_t m_vbo = 0;

	// deferred read-backs: one pixel-pack buffer holding every rectangle, and a fence after the reads
	struct pending_readback
	{
		uint32_t *out = nullptr;
		uint16_t *out16 = nullptr;
		int stride = 0;                 // of the destination, in its own pixels
		int w = 0, h = 0;               // output size
		size_t offset = 0;
	};
	std::vector<pending_readback> m_pending;
	uint32_t m_pbo = 0;
	size_t m_pbo_size = 0;
	size_t m_pending_bytes = 0;
	void *m_fence = nullptr;

	// final pass of a read-back (lookup, downsample, opaque alpha)
	struct output_program
	{
		uint32_t id = 0;
		int u_ds = -1, u_flags = -1, u_lut = -1, u_samples = -1, u_origin = -1;
	};
	output_program m_op[2];             // [1] reads the samples of a multisample target
	uint32_t m_op_vao = 0;
	uint32_t m_op_src_tex = 0, m_op_src_fbo = 0;
	int m_op_src_w = 0, m_op_src_h = 0;
	uint32_t m_op_dst_tex = 0, m_op_dst_fbo = 0;
	int m_op_dst_w = 0, m_op_dst_h = 0;

	// GPU timer query over the command loop, only when timers are enabled
	bool m_timers = false;
	uint32_t m_query = 0;
	bool m_query_pending = false;
	double m_last_gpu_ms = 0.0;

	void poll_gpu_timer(bool wait);
	bool valid_readback(const osd::gpu_readback_rect &r, const osd::gpu_target_desc &tg);
	void output_pass(const osd::gpu_readback_rect &r, void *dest, int row_length);
	bool ensure_output_program(bool per_sample);
	void fit_msaa(osd::gpu_target_desc &target);
	void do_readback(const osd::gpu_readback_rect &r, void *dest, int row_length);
	void drop_pending();

	int m_max_texture_samples = 0;
	uint64_t m_msaa_budget = 1024ULL * 1024 * 1024;
	int m_msaa_logged_requested = 0, m_msaa_logged_granted = 0;

	int m_problem_count = 0;
};

#endif // MAME_OSD_LIBRETRO_RETRO_GPU_ENGINE_H
