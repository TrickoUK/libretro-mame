// license:BSD-3-Clause
// copyright-holders:David Haywood
#ifndef MAME_KONAMI_K001005_H
#define MAME_KONAMI_K001005_H

#pragma once

#include "video/poly.h"
#include "cpu/sharc/sharc.h"
#include "k001006.h"
#include "video/rgbutil.h"

#include "interface/gpuengine.h"
#include "k001005_shader.h"

#include <cfloat>
#include <vector>


struct k001005_polydata
{
	int texture_x, texture_y;
	int texture_width, texture_height;
	int texture_page;
	int texture_palette;
	bool texture_mirror;
	rgb_t poly_color;
	rgb_t diffuse_light;
	rgb_t ambient_light;
	rgb_t fog_color;
	uint32_t cmd;
	bool fog_enable;
};

// vertex layout of the host-GPU path (see osd/interface/gpuengine.h)
struct k001005_gpu_vertex
{
	float x, y, z, w;
	uint32_t prim;
	float v[K001005_SHADER_VARYINGS];
};

class k001005_renderer : public poly_manager<float, k001005_polydata, 10>
{
	friend class k001005_device;

public:
	k001005_renderer(device_t &parent, screen_device &screen, device_t *k001006);

	void reset();
	void push_data(uint32_t data);
	void swap_buffers();
	bool fifo_filled();
	void draw(bitmap_rgb32 &bitmap, const rectangle &cliprect);
	int supersample() const { return m_ss; }
	bool output_scaled() const { return m_output_scaled; }

	int parse_polygon(int index, uint32_t cmd);
	void render_polygons();

	template<bool UseTexture, bool UseVertexColor>
	void draw_scanline_generic(int32_t scanline, const extent_t& extent, const k001005_polydata& extradata, int threadid);

	static constexpr int POLY_Z = 0;
	static constexpr int POLY_FOG = 1;
	static constexpr int POLY_DIFF = 2;
	static constexpr int POLY_U = 3;
	static constexpr int POLY_V = 4;
	static constexpr int POLY_W = 5;
	static constexpr int POLY_R = 6;
	static constexpr int POLY_G = 7;
	static constexpr int POLY_B = 8;
	static constexpr int POLY_A = 9;

private:
	// The 3D layer is 512x384. m_ss is the host-GPU supersampling factor (1 = native, and always 1 without an
	// engine): the GPU framebuffers, depth buffer and polygon coordinates are m_ss times bigger and draw() averages
	// a GPU-drawn page down. m_draw_ss is the scale the polygons of the current frame are drawn at.
	static constexpr int NATIVE_WIDTH = 512;
	static constexpr int NATIVE_HEIGHT = 384;
	int m_ss;
	int m_draw_ss;
	// With the 3D output scale the screen is presented at the supersampling size instead of
	// averaging the 3D layer down, so draw() copies samples one to one.
	bool m_output_scaled;
	// Not a hardware setting: bilinear filter textures even where the game doesn't ask for it
	bool m_force_bilinear;

	bitmap_rgb32 m_fb[2];
	bitmap_ind32 m_zb;
	rectangle m_cliprect;
	int m_fb_page;

	std::unique_ptr<uint32_t []> m_3dfifo;
	int m_3dfifo_ptr;

	vertex_t m_vertexb[4];
	int m_vertexb_ptr = 0;

	uint32_t m_light_r;
	uint32_t m_light_g;
	uint32_t m_light_b;
	uint32_t m_ambient_r;
	uint32_t m_ambient_g;
	uint32_t m_ambient_b;
	uint32_t m_fog_r;
	uint32_t m_fog_g;
	uint32_t m_fog_b;
	float m_far_z;
	float m_fog_start_z;
	float m_fog_end_z;
	uint16_t m_reg_fog_start;

	int16_t m_viewport_min_x;
	int16_t m_viewport_max_x;
	int16_t m_viewport_min_y;
	int16_t m_viewport_max_y;
	int16_t m_viewport_center_x;
	int16_t m_viewport_center_y;

	device_t *m_k001006;

	// host-GPU renderer (see k001005_shader.h); the engine is null when the path is off or has failed
	struct gpu_run
	{
		uint32_t first_vertex;
		uint32_t vertex_count;
		bool z_compare;
		bool fb_blend;
	};

	bool gpu_init();
	void gpu_add_polygon(k001005_polydata const &extra, rectangle const &cliprect, bool textured, bool vertex_color, bool is_quad, int v0, int v1, int v2, int v3);
	void gpu_submit();
	void gpu_collect();

	running_machine &m_gpu_machine;
	osd::gpu_engine *m_gpu_engine = nullptr;
	bool m_gpu_checked = false;
	bool m_gpu_full_upload = true;
	bool m_gpu_pending = false;             // a submitted frame has not been collected
	int m_gpu_msaa = 0;                     // multisample count for the GPU path (0 = off)
	// A page drawn by the GPU with multisampling carries the share of each pixel it covers in alpha
	// (255 = all) instead of the polygon alpha, and draw() blends partly covered pixels by it.
	bool m_page_share[2] = { false, false };
	bool m_page_native[2] = { false, false };   // the GPU averaged the page down to the native size
	osd::gpu_handle m_gpu_module = osd::GPU_NULL_HANDLE;
	osd::gpu_handle m_gpu_tex_rom = osd::GPU_NULL_HANDLE;
	osd::gpu_handle m_gpu_palette = osd::GPU_NULL_HANDLE;
	std::unique_ptr<uint32_t []> m_gpu_shadow_palette;
	std::vector<uint32_t> m_gpu_state;
	std::vector<k001005_gpu_vertex> m_gpu_vertices;
	std::vector<gpu_run> m_gpu_runs;

	std::unique_ptr<int[]> m_tex_mirror_table[2][8];
};


class k001005_device : public device_t, public device_video_interface
{
public:
	k001005_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock = 0);

	template <typename T> k001005_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock, T &&texel_tag)
		: k001005_device(mconfig, tag, owner, clock)
	{
		set_texel_tag(std::forward<T>(texel_tag));
	}
	template <typename T> k001005_device(const machine_config &mconfig, const char *tag, device_t *owner, T &&texel_tag)
		: k001005_device(mconfig, tag, owner, 0, std::forward<T>(texel_tag))
	{
		set_texel_tag(std::forward<T>(texel_tag));
	}


	template <typename T> void set_texel_tag(T &&tag) { m_k001006.set_tag(std::forward<T>(tag)); }

	void draw(bitmap_rgb32 &bitmap, const rectangle &cliprect);
	void swap_buffers();

	// With the 3D output scale the screen is presented at this multiple of its native size
	// (1 = native). The 2D layers still draw at native size.
	int output_scale() const { return m_output_scale; }

	// Draws a 2D layer through draw_native(bitmap, cliprect) at native size and, at an output scale
	// above 1, enlarges its opaque pixels (alpha set) onto the screen bitmap with sharp pixels.
	template <typename F> void draw_2d_layer(bitmap_rgb32 &bitmap, const rectangle &cliprect, F &&draw_native)
	{
		if (m_output_scale == 1)
		{
			draw_native(bitmap, cliprect);
			return;
		}

		rectangle const native_clip(
				cliprect.min_x / m_output_scale, cliprect.max_x / m_output_scale,
				cliprect.min_y / m_output_scale, cliprect.max_y / m_output_scale);
		m_layer.fill(0, native_clip);
		draw_native(m_layer, native_clip);
		enlarge_layer(bitmap, cliprect);
	}

	uint32_t read(address_space &space, offs_t offset, uint32_t mem_mask = ~0);
	void write(address_space &space, offs_t offset, uint32_t data, uint32_t mem_mask = ~0);

protected:
	// device-level overrides
	virtual void device_start() override ATTR_COLD;
	virtual void device_stop() override ATTR_COLD;
	virtual void device_reset() override ATTR_COLD;

private:
	void enlarge_layer(bitmap_rgb32 &bitmap, const rectangle &cliprect) const;

	// internal state
	optional_device<k001006_device> m_k001006;
	int m_output_scale = 1;
	bitmap_rgb32 m_layer;

	std::unique_ptr<uint16_t[]> m_ram[2];
	std::unique_ptr<uint32_t[]> m_fifo;
	uint32_t m_status;

	int m_ram_ptr;
	int m_fifo_read_ptr;
	int m_fifo_write_ptr;

	std::unique_ptr<k001005_renderer> m_renderer;
};

DECLARE_DEVICE_TYPE(K001005, k001005_device)

#endif // MAME_KONAMI_K001005_H
