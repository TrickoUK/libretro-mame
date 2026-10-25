// license:BSD-3-Clause
// copyright-holders:David Haywood
#include "emu.h"
#include "k001005.h"

#include "osdepend.h"
#include "screen.h"

#include "corefloat.h"

#include <cmath>

/*****************************************************************************/
/* Konami K001005 Polygon Renderer (KS10071) */

/***************************************************************************/
/*                                                                         */
/*                                  001005                                 */
/*                                                                         */
/***************************************************************************/

/*
    TODO:
        - Winding Heat (and maybe others) have slight Z-fighting problems.
        - Player car shadow not visible in Winding Heat. Hidden by road, needs polygon priority or something similar?

*/

k001005_renderer::k001005_renderer(device_t &parent, screen_device &screen, device_t *k001006)
	: poly_manager<float, k001005_polydata, 10>(screen.machine())
	, m_gpu_machine(screen.machine())
{
	m_k001006 = k001006;

	// Not a hardware setting: render at a multiple of the native size and average down
	int ss = screen.machine().osd().supersample();
	if (char const *env = getenv("MAME_K001005_SS"))
		ss = atoi(env);
	m_ss = std::clamp(ss, 1, 4);
	m_output_scaled = screen.machine().osd().supersample_output() && m_ss > 1;

	m_force_bilinear = screen.machine().osd().texture_filter();
	m_gpu_msaa = screen.machine().osd().msaa();
	if (char const *env = getenv("MAME_K001005_TEXFILTER"))
		m_force_bilinear = atoi(env) != 0;

	int const width = NATIVE_WIDTH * m_ss;
	int const height = NATIVE_HEIGHT * m_ss;

	m_fb[0].allocate(width, height);
	m_fb[1].allocate(width, height);

	m_zb.allocate(width, height);

	m_3dfifo = std::make_unique<uint32_t []>(0x10000);
	m_3dfifo_ptr = 0;
	m_fb_page = 0;
	m_light_r = 0;
	m_light_g = 0;
	m_light_b = 0;
	m_ambient_r = 0;
	m_ambient_g = 0;
	m_ambient_b = 0;
	m_fog_r = 0;
	m_fog_g = 0;
	m_fog_b = 0;
	m_far_z = 0;
	m_fog_start_z = 0;
	m_fog_end_z = 0;
	m_reg_fog_start = 0;
	m_viewport_min_x = 0;
	m_viewport_max_x = 0;
	m_viewport_min_y = 0;
	m_viewport_max_y = 0;
	m_viewport_center_x = 0;
	m_viewport_center_y = 0;

	m_cliprect = rectangle(0, width - 1, 0, height - 1);

	for (int k = 0; k < 8; k++)
	{
		m_tex_mirror_table[0][k] = std::make_unique<int []>(128);
		m_tex_mirror_table[1][k] = std::make_unique<int []>(128);

		int const size = (k + 1) * 8;

		for (int i = 0; i < 128; i++)
		{
			m_tex_mirror_table[0][k][i] = i % size;
			m_tex_mirror_table[1][k][i] = (i % (size * 2)) >= size ? ((size - 1) - (i % size)) : (i % size);
		}
	}

	// save state
	parent.save_pointer(NAME(m_3dfifo), 0x10000);
	parent.save_item(NAME(m_3dfifo_ptr));
	// The supersampled buffers are big, and the next frame redraws them anyway
	if (m_ss == 1)
	{
		parent.save_item(NAME(m_fb[0]));
		parent.save_item(NAME(m_fb[1]));
		parent.save_item(NAME(m_zb));
	}
	parent.save_item(NAME(m_fb_page));
	parent.save_item(NAME(m_light_r));
	parent.save_item(NAME(m_light_g));
	parent.save_item(NAME(m_light_b));
	parent.save_item(NAME(m_ambient_r));
	parent.save_item(NAME(m_ambient_g));
	parent.save_item(NAME(m_ambient_b));
	parent.save_item(NAME(m_fog_r));
	parent.save_item(NAME(m_fog_g));
	parent.save_item(NAME(m_fog_b));
	parent.save_item(NAME(m_far_z));
	parent.save_item(NAME(m_fog_start_z));
	parent.save_item(NAME(m_fog_end_z));
	parent.save_item(NAME(m_reg_fog_start));
	parent.save_item(NAME(m_viewport_min_x));
	parent.save_item(NAME(m_viewport_max_x));
	parent.save_item(NAME(m_viewport_min_y));
	parent.save_item(NAME(m_viewport_max_y));
	parent.save_item(NAME(m_viewport_center_x));
	parent.save_item(NAME(m_viewport_center_y));
}

void k001005_renderer::reset()
{
	m_3dfifo_ptr = 0;

	m_vertexb_ptr = 0;
}

void k001005_renderer::push_data(uint32_t data)
{
	// process the current vertex data if a sync command is being sent (usually means the global registers are being changed)
	if (data == 0x80000000)
	{
		render_polygons();
	}

	m_3dfifo[m_3dfifo_ptr++] = data;
}

void k001005_renderer::swap_buffers()
{
	// a frame still in flight belongs to the page about to be reused
	gpu_collect();

	// the polygons of the finished frame go to the GPU now, into the buffer they were drawn for
	if (!m_gpu_runs.empty())
		gpu_submit();

	m_fb_page ^= 1;

	// A GPU-drawn page is written by the read-back alone: the native-size area at native output scale, the whole
	// page otherwise. The depth buffer is only used by the software renderer.
	bool const gpu = gpu_init();
	bool const native_out = gpu && m_ss > 1 && !m_output_scaled;
	rectangle const clear = native_out ? rectangle(0, m_cliprect.width() / m_ss - 1, 0, m_cliprect.height() / m_ss - 1) : m_cliprect;
	m_fb[m_fb_page].fill(0, clear);
	m_page_share[m_fb_page] = false;
	m_page_native[m_fb_page] = native_out;

	if (!gpu)
	{
		float const zvalue = 10000000000.0F;
		m_zb.fill(*(int*)&zvalue, m_cliprect);
	}
}

bool k001005_renderer::fifo_filled()
{
	return m_3dfifo_ptr > 0;
}


bool k001005_renderer::gpu_init()
{
	if (!m_gpu_checked)
	{
		m_gpu_checked = true;
		k001006_device *const k001006 = downcast<k001006_device *>(m_k001006);
		m_gpu_engine = m_gpu_machine.osd().get_gpu_engine("k001005");
		if (m_gpu_engine && (!k001006 || k001006->texture_page_count() * 512 > m_gpu_engine->caps().max_texture_size))
			m_gpu_engine = nullptr;
		if (m_gpu_engine)
		{
			osd::gpu_shader_module module;
			module.name = "k001005";
			module.fragment_source = K001005_FRAGMENT_SOURCE;
			if (m_gpu_msaa > 0)
				module.centroid_mask = 0x1ff;
			module.varying_count = K001005_SHADER_VARYINGS;
			for (int i = 0; i < K001005_SHADER_VARYINGS; i++)
				module.varying_mode[i] = osd::gpu_varying_mode::NOPERSPECTIVE;
			module.state_stride = K001005_SHADER_STATE_WORDS;
			module.binding_count = 2;
			module.binding_name[0] = "tex_rom";
			module.binding_format[0] = osd::gpu_texture_format::R8UI;
			module.binding_name[1] = "palette";
			module.binding_format[1] = osd::gpu_texture_format::R32UI;
			m_gpu_module = m_gpu_engine->create_shader_module(module);

			osd::gpu_texture_desc desc;
			desc.format = osd::gpu_texture_format::R8UI;
			desc.width = 512;
			desc.height = k001006->texture_page_count() * 512;
			m_gpu_tex_rom = m_gpu_engine->create_texture(desc);
			desc.format = osd::gpu_texture_format::R32UI;
			desc.width = 256;
			desc.height = 8;
			m_gpu_palette = m_gpu_engine->create_texture(desc);

			if (!m_gpu_module || !m_gpu_tex_rom || !m_gpu_palette)
				m_gpu_engine = nullptr;
			else
				m_gpu_shadow_palette = make_unique_clear<uint32_t []>(0x800);
		}
	}
	return m_gpu_engine != nullptr && !m_gpu_engine->failed();
}

// Queues the triangles of one polygon, with the state the scanline routine would have read.
void k001005_renderer::gpu_add_polygon(k001005_polydata const &extra, rectangle const &cliprect, bool textured, bool vertex_color, bool is_quad, int v0, int v1, int v2, int v3)
{
	k001006_device *const k001006 = downcast<k001006_device *>(m_k001006);
	bool const bilinear = m_force_bilinear || k001006->bilinear_enabled();
	bool const z_compare = BIT(extra.cmd, 2);
	bool const fb_blend = BIT(~extra.cmd, 1);

	uint32_t const prim = m_gpu_state.size() / K001005_SHADER_STATE_WORDS;
	uint32_t const flags = (textured ? 1 : 0) | (vertex_color ? 2 : 0) | (extra.fog_enable ? 4 : 0) | (bilinear ? 8 : 0)
			| (extra.texture_mirror ? 16 : 0) | (fb_blend ? 32 : 0);
	m_gpu_state.insert(m_gpu_state.end(), {
			flags,
			uint32_t(extra.texture_x * 8),
			uint32_t(extra.texture_y * 8),
			uint32_t(extra.texture_width),
			uint32_t(extra.texture_height),
			uint32_t(extra.texture_page),
			uint32_t(extra.texture_palette * 256),
			uint32_t(extra.diffuse_light),
			uint32_t(extra.ambient_light),
			uint32_t(extra.fog_color),
			uint32_t(extra.poly_color),
			uint32_t(cliprect.min_x),
			uint32_t(cliprect.min_y),
			uint32_t(cliprect.max_x),
			uint32_t(cliprect.max_y),
			0 });

	// quads are two triangles, other polygons the triangle of the last three vertices
	int const quad[6] = { v0, v1, v2, v2, v3, v0 };
	int const strip[3] = { v1, v2, v3 };
	int const *const order = is_quad ? quad : strip;
	int const vertex_count = is_quad ? 6 : 3;

	uint32_t const start = m_gpu_vertices.size();
	for (int i = 0; i < vertex_count; i++)
	{
		vertex_t const &src = m_vertexb[order[i]];
		k001005_gpu_vertex &dst = m_gpu_vertices.emplace_back();
		dst.x = src.x;
		dst.y = src.y;
		dst.z = src.p[POLY_Z];
		dst.w = 1.0F;
		dst.prim = prim;
		dst.v[0] = src.p[POLY_DIFF];
		dst.v[1] = src.p[POLY_FOG];
		dst.v[2] = textured ? src.p[POLY_U] : 0.0F;
		dst.v[3] = textured ? src.p[POLY_V] : 0.0F;
		dst.v[4] = src.p[POLY_W];
		dst.v[5] = vertex_color ? src.p[POLY_R] : 0.0F;
		dst.v[6] = vertex_color ? src.p[POLY_G] : 0.0F;
		dst.v[7] = vertex_color ? src.p[POLY_B] : 0.0F;
		dst.v[8] = vertex_color ? src.p[POLY_A] : 0.0F;
	}

	// consecutive polygons with the same depth test and blend state share a draw
	uint32_t const added = m_gpu_vertices.size() - start;
	if (!m_gpu_runs.empty() && m_gpu_runs.back().z_compare == z_compare && m_gpu_runs.back().fb_blend == fb_blend)
		m_gpu_runs.back().vertex_count += added;
	else
		m_gpu_runs.push_back({ start, added, z_compare, fb_blend });
}

// Draws the queued polygons into the frame buffer the software renderer would have drawn them into.
void k001005_renderer::gpu_submit()
{
	k001006_device *const k001006 = downcast<k001006_device *>(m_k001006);

	// depth is a linear remap of the vertex z to the range the GPU compares
	float zmin = FLT_MAX, zmax = -FLT_MAX;
	for (k001005_gpu_vertex const &vertex : m_gpu_vertices)
	{
		if (std::isfinite(vertex.z))
		{
			zmin = std::min(zmin, vertex.z);
			zmax = std::max(zmax, vertex.z);
		}
	}
	float const zscale = (zmax > zmin) ? (0.99F / (zmax - zmin)) : 0.0F;
	for (k001005_gpu_vertex &vertex : m_gpu_vertices)
		vertex.z = std::isfinite(vertex.z) ? (0.005F + (vertex.z - zmin) * zscale) : 0.995F;

	// the K001006 pens change at any time, the texture pages never do
	osd::gpu_texture_update updates[2];
	int update_count = 0;
	if (m_gpu_full_upload)
	{
		osd::gpu_texture_update &u = updates[update_count++];
		u.texture = m_gpu_tex_rom;
		u.w = 512;
		u.h = k001006->texture_page_count() * 512;
		u.data = k001006->texture_pages();
		u.pitch_bytes = 512;
	}
	{
		uint32_t pens[0x800];
		for (int i = 0; i < 0x800; i++)
			pens[i] = k001006->pen_color(i);
		if (m_gpu_full_upload || memcmp(pens, m_gpu_shadow_palette.get(), sizeof(pens)))
		{
			memcpy(m_gpu_shadow_palette.get(), pens, sizeof(pens));
			osd::gpu_texture_update &u = updates[update_count++];
			u.texture = m_gpu_palette;
			u.w = 256;
			u.h = 8;
			u.data = m_gpu_shadow_palette.get();
			u.pitch_bytes = 256 * sizeof(uint32_t);
		}
	}
	m_gpu_full_upload = false;

	std::vector<osd::gpu_command> commands;
	for (gpu_run const &run : m_gpu_runs)
	{
		osd::gpu_command draw = osd::gpu_command::make_draw(m_gpu_module, run.first_vertex, run.vertex_count);
		draw.variant = osd::gpu_aniso_variant(m_gpu_machine.osd().anisotropy());
		draw.bindings[0] = m_gpu_tex_rom;
		draw.bindings[1] = m_gpu_palette;
		draw.depth_func = run.z_compare ? osd::gpu_depth_func::LEQUAL : osd::gpu_depth_func::ALWAYS;
		draw.depth_write = true;
		// only geometry with a depth compare has its edges multisampled
		draw.multisample = run.z_compare;
		if (run.fb_blend)
		{
			draw.blend.enable = true;
			draw.blend.src = osd::gpu_blend_factor::SRC_ALPHA;
			draw.blend.dst = osd::gpu_blend_factor::ONE_MINUS_SRC_ALPHA;
		}
		commands.push_back(draw);
	}

	bitmap_rgb32 &target = m_fb[m_fb_page];
	osd::gpu_readback_rect readback;
	readback.x = 0;
	readback.y = 0;
	readback.w = target.width();
	readback.h = target.height();
	readback.out = &target.pix(0, 0);
	readback.stride = target.rowpixels();
	// Rendering at a multiple of the native size without an output scale: the GPU averages each block of samples
	// down to one native pixel, into the top-left of the page. The layer is transparent where nothing was drawn,
	// so the samples of an edge pixel are averaged over the drawn ones only, with the drawn share in alpha.
	bool const native_out = m_ss > 1 && !m_output_scaled;
	if (native_out)
		readback.downsample = m_ss;
	if (native_out || m_gpu_msaa > 0)
	{
		readback.coverage = true;
		readback.coverage_share = true;
	}

	osd::gpu_frame frame;
	frame.target.width = target.width();
	frame.target.height = target.height();
	frame.target.depth = osd::gpu_depth_mode::REAL;
	frame.target.msaa_samples = m_gpu_msaa;
	frame.target.msaa_per_sample = m_gpu_msaa > 0;
	frame.updates = updates;
	frame.update_count = update_count;
	frame.vertices = m_gpu_vertices.data();
	frame.vertex_count = m_gpu_vertices.size();
	frame.varyings_per_vertex = K001005_SHADER_VARYINGS;
	frame.prim_state = m_gpu_state.data();
	frame.prim_count = m_gpu_state.size() / K001005_SHADER_STATE_WORDS;
	frame.state_stride = K001005_SHADER_STATE_WORDS;
	frame.commands = commands.data();
	frame.command_count = commands.size();
	frame.readbacks = &readback;
	frame.readback_count = 1;
	frame.defer_readback = true;
	if (m_gpu_engine->submit_frame(frame))
	{
		m_gpu_pending = true;
		m_page_share[m_fb_page] = native_out || m_gpu_msaa > 0;
		m_page_native[m_fb_page] = native_out;
	}
	else
	{
		// the textures may not have reached the GPU, and this frame stays empty
		m_gpu_full_upload = true;
		m_fb[m_fb_page].fill(0, m_cliprect);
		m_page_native[m_fb_page] = false;
	}

	m_gpu_state.clear();
	m_gpu_vertices.clear();
	m_gpu_runs.clear();
}

// Waits for the frame submitted at the last buffer swap and fills the page it was drawn for.
void k001005_renderer::gpu_collect()
{
	if (!m_gpu_pending)
		return;
	m_gpu_pending = false;
	if (!m_gpu_engine->collect_frame())
		m_gpu_full_upload = true;
}


template<bool UseTexture, bool UseVertexColor>
void k001005_renderer::draw_scanline_generic(int32_t scanline, const extent_t& extent, const k001005_polydata& extradata, int threadid)
{
	k001006_device* k001006 = downcast<k001006_device*>(m_k001006);

	uint32_t *const fb = &m_fb[m_fb_page].pix(scanline);
	float *const zb = (float*)&m_zb.pix(scanline);

	float z = extent.param[POLY_Z].start;
	float const dz = extent.param[POLY_Z].dpdx;
	float diff = extent.param[POLY_DIFF].start;
	float const ddiff = extent.param[POLY_DIFF].dpdx;
	float fog = extent.param[POLY_FOG].start;
	float const dfog = extent.param[POLY_FOG].dpdx;

	float u, v, w, du, dv, dw;
	if (UseTexture)
	{
		u = extent.param[POLY_U].start;
		v = extent.param[POLY_V].start;
		w = extent.param[POLY_W].start;
		du = extent.param[POLY_U].dpdx;
		dv = extent.param[POLY_V].dpdx;
		dw = extent.param[POLY_W].dpdx;
	}

	float r, g, b, a, dr, dg, db, da;
	if (UseVertexColor)
	{
		r = extent.param[POLY_R].start;
		dr = extent.param[POLY_R].dpdx;
		g = extent.param[POLY_G].start;
		dg = extent.param[POLY_G].dpdx;
		b = extent.param[POLY_B].start;
		db = extent.param[POLY_B].dpdx;
		a = extent.param[POLY_A].start;
		da = extent.param[POLY_A].dpdx;
	}

	rgbaint_t ambient_color(extradata.ambient_light);
	rgbaint_t diffuse_color(extradata.diffuse_light);
	rgbaint_t fog_color(extradata.fog_color);

	rgbaint_t poly_color(extradata.poly_color);
	int const poly_color_a = (extradata.poly_color >> 24) & 0xff;

	int const texture_mirror_x = extradata.texture_mirror;
	int const texture_mirror_y = extradata.texture_mirror;
	int const texture_x = extradata.texture_x * 8;
	int const texture_y = extradata.texture_y * 8;
	int const texture_width = extradata.texture_width;
	int const texture_height = extradata.texture_height;
	int const tex_page = extradata.texture_page * 0x40000;
	int const palette_index = extradata.texture_palette * 256;

	int const *const x_mirror_table = m_tex_mirror_table[texture_mirror_x][texture_width].get();
	int const *const y_mirror_table = m_tex_mirror_table[texture_mirror_y][texture_height].get();

	bool const UseZCompare = BIT(extradata.cmd, 2);
	bool const UseFBBlend = BIT(~extradata.cmd, 1);
	bool const UseFog = extradata.fog_enable;

	bool const WriteZ = true;
	bool const UseBilinear = m_force_bilinear || k001006->bilinear_enabled();

	uint32_t texel = 0;
	uint32_t texel_alpha = 0;

	for (int x = extent.startx; x < extent.stopx; x++)
	{
		if (z <= zb[x] || !UseZCompare)
		{
			if (UseTexture)
			{
				float const oow = 1.0F / w;

				int iu = int(u * oow);
				int iv = int(v * oow);

				if (!UseBilinear)
				{
					int const texel_u = texture_x + x_mirror_table[(iu >> 4) & 0x7f];
					int const texel_v = texture_y + y_mirror_table[(iv >> 4) & 0x7f];

					texel = k001006->fetch_texel(tex_page, palette_index, texel_u, texel_v);
					texel_alpha = texel >> 24;
				}
				else
				{
					// sub-texel bias to avoid seams
					iu -= 7;
					iv -= 7;

					int const ufrac = iu & 0xf;
					int const vfrac = iv & 0xf;
					int const texel_u0 = texture_x + x_mirror_table[(iu >> 4) & 0x7f];
					int const texel_u1 = texture_x + x_mirror_table[((iu >> 4) + 1) & 0x7f];
					int const texel_v0 = texture_y + y_mirror_table[(iv >> 4) & 0x7f];
					int const texel_v1 = texture_y + y_mirror_table[((iv >> 4) + 1) & 0x7f];

					uint32_t const tex00 = k001006->fetch_texel(tex_page, palette_index, texel_u0, texel_v0);
					uint32_t const tex01 = k001006->fetch_texel(tex_page, palette_index, texel_u1, texel_v0);
					uint32_t const tex10 = k001006->fetch_texel(tex_page, palette_index, texel_u0, texel_v1);
					uint32_t const tex11 = k001006->fetch_texel(tex_page, palette_index, texel_u1, texel_v1);

					// The hardware appears to keep the silhouette of cut-outs: transparency comes from the
					// nearest texel, and transparent neighbours borrow its colour instead of bleeding black
					uint32_t tex[4] = { tex00, tex01, tex10, tex11 };
					uint32_t const nearest = tex[((vfrac >= 9) ? 2 : 0) | ((ufrac >= 9) ? 1 : 0)];
					texel_alpha = nearest >> 24;
					if (texel_alpha != 0)
					{
						for (uint32_t &t : tex)
						{
							if ((t >> 24) == 0)
								t = nearest;
						}
					}
					texel = rgbaint_t::bilinear_filter(tex[0], tex[1], tex[2], tex[3], ufrac * 16, vfrac * 16);
				}
			}

			int const idiff = std::clamp((int)(diff), 0, 255);
			int const ifog = std::clamp((int)(fog), 0, 255);

			rgbaint_t light_color(extradata.diffuse_light);
			light_color.scale_imm_and_clamp(idiff);
			light_color.add(ambient_color);
			light_color.clamp_to_uint8();

			if (UseVertexColor)
			{
				int const ir = std::clamp((int)(r), 0, 255);
				int const ig = std::clamp((int)(g), 0, 255);
				int const ib = std::clamp((int)(b), 0, 255);
				int const ia = std::clamp((int)(a), 0, 255);

				if (ia != 0)
				{
					rgbaint_t color(ia, ir, ig, ib);

					if (UseTexture && texel_alpha != 0)
					{
						color.set(texel);
					}

					color.scale_and_clamp(light_color);

					if (UseFog)
					{
						color.blend(fog_color, ifog);
					}

					// framebuffer blend
					if (UseFBBlend)
					{
						rgbaint_t fb_color(fb[x]);
						color.blend(fb_color, ia);
					}

					fb[x] = color.to_rgba();
					if (WriteZ)
						zb[x] = z;
				}
			}
			else
			{
				if (UseTexture)
				{
					if (texel_alpha != 0)
					{
						rgbaint_t texel_color(texel);
						texel_color.scale_and_clamp(light_color);

						// TODO: is there a toggle for texture blending? cmd bit 0x02 doesn't seem like it
						if (UseBilinear && texel_alpha < 0xff)
						{
							rgbaint_t fb_color(fb[x]);
							texel_color.blend(fb_color, texel_alpha);
						}

						if (UseFog)
						{
							texel_color.blend(fog_color, ifog);
						}

						if (UseFBBlend)
						{
							rgbaint_t fb_color(fb[x]);
							texel_color.blend(fb_color, poly_color_a);
						}

						fb[x] = texel_color.to_rgba();
						if (WriteZ)
							zb[x] = z;
					}
				}
				else
				{
					//if (poly_color_a != 0)
					{
						rgbaint_t color(extradata.poly_color);
						color.scale_and_clamp(light_color);

						if (UseFog)
						{
							color.blend(fog_color, ifog);
						}

						// framebuffer blend
						if (UseFBBlend)
						{
							rgbaint_t fb_color(fb[x]);
							color.blend(fb_color, poly_color_a);
						}

						fb[x] = color.to_rgba();
						if (WriteZ)
							zb[x] = z;
					}
				}
			}

		}

		z += dz;
		diff += ddiff;
		fog += dfog;

		if (UseVertexColor)
		{
			r += dr;
			g += dg;
			b += db;
			a += da;
		}

		if (UseTexture)
		{
			u += du;
			v += dv;
			w += dw;
		}
	}
}


/*
    Command
    0x00: xxxxxxxx xxxxxxxx xxxxxxx- --------    0x80000000 (exact number of bits unknown)
    0x00: -------- -------- -------x --------    0 = per-poly color, 1 = per-vertex color
    0x00: -------- -------- -------- x-------    ? Texture related
    0x00: -------- -------- -------- -x------    Unused?
    0x00: -------- -------- -------- --x-----    1 = enable smooth shading?
    0x00: -------- -------- -------- ---x----    1 = texture mirroring
    0x00: -------- -------- -------- ----x---    ? Texture related
    0x00: -------- -------- -------- -----x--    1 = enable Z-buffer read
    0x00: -------- -------- -------- ------x-    0 = blend enabled, 1 = disabled
    0x00: -------- -------- -------- -------x    0 = per-vertex Z, 1 = per-poly Z (0x80000121 seems like an exception)

    Texture header
    0x01: -xxx---- -------- -------- --------    Texture palette
    0x01: ----xx-- -------- -------- --------    Unknown flags, set by commands 0x7b...0x7e. Used mostly on polygons further away from camera. Some kind of depth-based effect?
    0x01: ------xx x------- -------- --------    Texture width / 8 - 1
    0x01: -------- -xxx---- -------- --------    Texture height / 8 - 1
    0x01: -------- -------x xxxx---- --------    Texture page
    0x01: -------- -------- ----x-x- x-x-x-x-    Texture X / 8
    0x01: -------- -------- -----x-x -x-x-x-x    Texture Y / 8
*/

int k001005_renderer::parse_polygon(int index, uint32_t cmd)
{
	render_delegate rd_scan_tex = render_delegate(&k001005_renderer::draw_scanline_generic<true, false>, this);
	render_delegate rd_scan_vertex_color = render_delegate(&k001005_renderer::draw_scanline_generic<false, true>, this);
	render_delegate rd_scan_vertex_color_tex = render_delegate(&k001005_renderer::draw_scanline_generic<true, true>, this);
	render_delegate rd_scan_color = render_delegate(&k001005_renderer::draw_scanline_generic<false, false>, this);

	int const viewport_min_x = std::clamp(256 + m_viewport_min_x + m_viewport_center_x, 0, NATIVE_WIDTH - 1);
	int const viewport_max_x = std::clamp(256 + m_viewport_max_x + m_viewport_center_x + 1, 0, NATIVE_WIDTH - 1);
	int const viewport_min_y = std::clamp(200 + m_viewport_min_y - m_viewport_center_y, 0, NATIVE_HEIGHT - 1);
	int const viewport_max_y = std::clamp(200 + m_viewport_max_y - m_viewport_center_y + 1, 0, NATIVE_HEIGHT - 1);

	// The clip rectangle is inclusive, so the last native pixel covers up to the last sample
	rectangle cliprect(viewport_min_x * m_ss, (viewport_max_x + 1) * m_ss - 1, viewport_min_y * m_ss, (viewport_max_y + 1) * m_ss - 1);

	int const start_index = index;

	uint32_t const *const fifo = m_3dfifo.get();

	bool const gpu = gpu_init();

	bool const has_texture = (cmd & 0x18) != 0;
	bool const has_vertex_color = BIT(cmd, 8);
	bool const has_vertex_z = BIT(~cmd, 0) || has_vertex_color;     // command 0x121 breaks the logic here, maybe vertex color enforces vertex z too?

	uint32_t texture_x = 0;
	uint32_t texture_y = 0;
	uint32_t texture_width = 0;
	uint32_t texture_height = 0;
	uint32_t texture_page = 0;
	uint32_t texture_palette = 0;

	uint32_t tex_header = 0;

	// texture header - only for textured polys
	if (has_texture)
	{
		tex_header = fifo[index++];

		texture_x = (((tex_header >> 6) & 0x20) | ((tex_header >> 5) & 0x10) | ((tex_header >> 4) & 0x8) |
					((tex_header >> 3) & 0x4) | ((tex_header >> 2) & 0x2) | ((tex_header >> 1) & 0x1));

		texture_y = (((tex_header >> 5) & 0x20) | ((tex_header >> 4) & 0x10) | ((tex_header >> 3) & 0x8) |
					((tex_header >> 2) & 0x4) | ((tex_header >> 1) & 0x2) | (tex_header & 0x1));

		texture_width = (tex_header >> 23) & 0x7;
		texture_height = (tex_header >> 20) & 0x7;
		texture_page = (tex_header >> 12) & 0x1f;
		texture_palette = (tex_header >> 28) & 0x7;
	}

	while ((fifo[index] & 0xffff0000) != 0x80000000 && index < m_3dfifo_ptr)
	{
		k001005_polydata& extra = object_data().next();

		bool last_vertex = false;
		bool is_quad = false;

		uint32_t polygon_color = 0;
		float polygon_z = 0.0f;
		uint32_t polygon_diffuse = 0;

		int num_new_verts = 0;

		do
		{
			// X/Y coords, flags - all polys have this
			// -------------------------------------------------------------------------
			int x = fifo[index] & 0x3fff;
			x |= ((x & 0x2000) ? 0xffffc000 : 0);
			int y = (fifo[index] >> 16) & 0x1fff;
			y |= ((y & 0x1000) ? 0xffffe000 : 0);

			m_vertexb[m_vertexb_ptr].x = (((float)(x) / 16.0f) + 256.0f) * m_ss;
			m_vertexb[m_vertexb_ptr].y = (((float)(-y) / 16.0f) + 200.0f) * m_ss;

			is_quad = BIT(fifo[index], 14);
			last_vertex = BIT(fifo[index], 15);
			index++;

			// Z + diffuse intensity - if Z enabled
			// -------------------------------------------------------------------------
			if (has_vertex_z)
			{
				uint32_t const z = fifo[index] & 0xffffff00;      // 32-bit float with low 8-bits of mantissa masked out
				int const diffuse = fifo[index] & 0xff;
				index++;

				m_vertexb[m_vertexb_ptr].p[POLY_Z] = u2f(z);
				m_vertexb[m_vertexb_ptr].p[POLY_DIFF] = diffuse;
				m_vertexb[m_vertexb_ptr].p[POLY_W] = 1.0f / m_vertexb[m_vertexb_ptr].p[POLY_Z];
			}
			else
			{
				m_vertexb[m_vertexb_ptr].p[POLY_W] = 1.0f;
			}

			// textured polygons have a polygon color field after last vertex data, but before the last UV coords
			// -------------------------------------------------------------------------
			if (last_vertex && has_texture)
			{
				// polygon Z comes before the last UV coords for textured polygons
				if (!has_vertex_z)
				{
					uint32_t const z = (fifo[index] & 0x07ffff00) | 0x48000000;   // like fog values, these seem to be missing the 4 upper bits of exponent
					polygon_diffuse = fifo[index] & 0xff;
					index++;
					polygon_z = u2f(z);
				}

				if (!has_vertex_color)
				{
					polygon_color = fifo[index];
					index++;
				}
			}

			// vertex color
			if (has_vertex_color)
			{
				uint32_t const vertex_color = fifo[index];
				index++;

				m_vertexb[m_vertexb_ptr].p[POLY_A] = (vertex_color >> 24) & 0xff;
				m_vertexb[m_vertexb_ptr].p[POLY_B] = (vertex_color >> 16) & 0xff;
				m_vertexb[m_vertexb_ptr].p[POLY_G] = (vertex_color >> 8) & 0xff;
				m_vertexb[m_vertexb_ptr].p[POLY_R] = vertex_color & 0xff;

			}

			// UV coords - only for texture polys
			if (has_texture)
			{
				int32_t const tu = (int16_t)(fifo[index] >> 16);
				int32_t const tv = (int16_t)(fifo[index] & 0xffff);
				index++;

				m_vertexb[m_vertexb_ptr].p[POLY_U] = float(tu) * m_vertexb[m_vertexb_ptr].p[POLY_W];
				m_vertexb[m_vertexb_ptr].p[POLY_V] = float(tv) * m_vertexb[m_vertexb_ptr].p[POLY_W];
			}

			// fog
			if (m_reg_fog_start == 0xffff)
			{
				// max fog start value means fog is off
				m_vertexb[m_vertexb_ptr].p[POLY_FOG] = 0.0f;
			}
			else
			{
				float const fog_factor = (m_fog_end_z - m_vertexb[m_vertexb_ptr].p[POLY_Z]) / (m_fog_end_z - m_fog_start_z);
				m_vertexb[m_vertexb_ptr].p[POLY_FOG] = fog_factor * 255.0f;
			}

			num_new_verts++;
			m_vertexb_ptr = (m_vertexb_ptr + 1) & 3;
		}
		while (!last_vertex && num_new_verts < 4);

		// for non-textured polygons, polygon color comes after vertex data
		if (!has_texture)
		{
			// polygon Z
			if (!has_vertex_z)
			{
				uint32_t const z = (fifo[index] & 0x07ffff00) | 0x48000000;   // like fog values, these seem to be missing the 4 upper bits of exponent
				polygon_diffuse = fifo[index] & 0xff;
				index++;

				polygon_z = u2f(z);
			}

			// polygon color
			if (!has_vertex_color)
			{
				polygon_color = fifo[index];
				index++;
			}
		}

		// apply constant Z to all verts if needed
		if (!has_vertex_z)
		{
			for (auto j = 0; j < 4; j++)
			{
				m_vertexb[j].p[POLY_Z] = polygon_z;
				m_vertexb[j].p[POLY_DIFF] = polygon_diffuse;
			}
		}

		extra.texture_x = texture_x;
		extra.texture_y = texture_y;
		extra.texture_width = texture_width;
		extra.texture_height = texture_height;
		extra.texture_page = texture_page;
		extra.texture_palette = texture_palette;
		extra.texture_mirror = (cmd & 0x10);
		extra.diffuse_light = rgb_t(m_light_r, m_light_g, m_light_b);
		extra.ambient_light = rgb_t(m_ambient_r, m_ambient_g, m_ambient_b);
		extra.fog_color = rgb_t(m_fog_r, m_fog_g, m_fog_b);
		extra.fog_enable = (m_reg_fog_start != 0xffff) && !(cmd & 1);
		extra.cmd = cmd;

		extra.poly_color = rgb_t((polygon_color >> 24) & 0xff, polygon_color & 0xff, (polygon_color >> 8) & 0xff, (polygon_color >> 16) & 0xff);


		// If 4 new vertices were found, but no last vertex tag - we're reading garbage.
		// Midnrun writes garbage after a 0x80000003 command. The data comes directly from the display list, so it seems intentional.
		if (num_new_verts >= 4 && !last_vertex)
			break;


		// The vertex buffer is a 4-entry circular buffer.
		// Each polygon has at least one new vertex. 0-3 vertices are reused based on how many new vertices were inserted.
		int const v0 = (m_vertexb_ptr - 4) & 3;
		int const v1 = (m_vertexb_ptr - 3) & 3;
		int const v2 = (m_vertexb_ptr - 2) & 3;
		int const v3 = (m_vertexb_ptr - 1) & 3;


		// This fixes shading issues in the Konami logo in Solar Assault.
		// Some triangle strips have different shading values compared to reused vertices, causing unintended smooth shading.
		// This ensures all vertices have the same shading value.
		// Bit 0x20 could be a select between flat shading and gouraud shading.
		if (BIT(~cmd, 5) && num_new_verts < 3)
		{
			int const last_diffuse = m_vertexb[v3].p[POLY_DIFF];
			m_vertexb[v0].p[POLY_DIFF] = last_diffuse;
			m_vertexb[v1].p[POLY_DIFF] = last_diffuse;
			m_vertexb[v2].p[POLY_DIFF] = last_diffuse;
		}

		// No texture, constant color:   Z, Fog, Diffuse
		// Texture, constant color:      Z, Fog, Diffuse, U, V, W
		// Per-vertex color:             Z, Fog, Diffuse, U, V, W, R, G, B, A
		if (gpu)
		{
			gpu_add_polygon(extra, cliprect, has_texture, has_vertex_color, is_quad, v0, v1, v2, v3);
		}
		else if (is_quad)
		{
			if (has_vertex_color)
			{
				render_triangle<10>(cliprect, has_texture ? rd_scan_vertex_color_tex : rd_scan_vertex_color, m_vertexb[v0], m_vertexb[v1], m_vertexb[v2]);
				render_triangle<10>(cliprect, has_texture ? rd_scan_vertex_color_tex : rd_scan_vertex_color, m_vertexb[v2], m_vertexb[v3], m_vertexb[v0]);
			}
			else if (has_texture)
			{
				render_triangle<6>(cliprect, rd_scan_tex, m_vertexb[v0], m_vertexb[v1], m_vertexb[v2]);
				render_triangle<6>(cliprect, rd_scan_tex, m_vertexb[v2], m_vertexb[v3], m_vertexb[v0]);
			}
			else
			{
				render_triangle<3>(cliprect, rd_scan_color, m_vertexb[v0], m_vertexb[v1], m_vertexb[v2]);
				render_triangle<3>(cliprect, rd_scan_color, m_vertexb[v2], m_vertexb[v3], m_vertexb[v0]);
			}
		}
		else
		{
			if (has_vertex_color)
			{
				render_triangle<10>(cliprect, has_texture ? rd_scan_vertex_color_tex : rd_scan_vertex_color, m_vertexb[v1], m_vertexb[v2], m_vertexb[v3]);
			}
			else if (has_texture)
			{
				render_triangle<6>(cliprect, rd_scan_tex, m_vertexb[v1], m_vertexb[v2], m_vertexb[v3]);
			}
			else
			{
				render_triangle<3>(cliprect, rd_scan_color, m_vertexb[v1], m_vertexb[v2], m_vertexb[v3]);
			}
		}
	}
	return index - start_index;
}


void k001005_renderer::render_polygons()
{
	uint32_t const *const fifo = m_3dfifo.get();
	int index = 0;

	do
	{
		uint32_t const cmd = fifo[index++];

		if (cmd == 0x80000000 || cmd == 0x80000018)
		{
		}
		else if ((cmd & 0xffff0000) == 0x80000000)
		{
			index += parse_polygon(index, cmd);
		}
	}
	while (index < m_3dfifo_ptr);

	m_3dfifo_ptr = 0;
	wait();
}


void k001005_renderer::draw(bitmap_rgb32 &bitmap, const rectangle &cliprect)
{
	gpu_collect();

	// a page the GPU averaged already has one pixel per native pixel
	int const ss = m_page_native[m_fb_page ^ 1] ? 1 : m_ss;
	int const samples = ss * ss;
	bitmap_rgb32 const &page = m_fb[m_fb_page ^ 1];

	// A page drawn with multisampling holds the average colour of the drawn samples of each pixel and the
	// share of the pixel they cover in alpha. A partly covered pixel is blended over what is behind the layer
	// by that share, which anti-aliases the edge.
	bool const share = m_page_share[m_fb_page ^ 1];

	// Blends the colour channels of src over back, share_sum out of share_max of the pixel being drawn
	auto const blend = [] (uint32_t src, uint32_t back, uint32_t share_sum, uint32_t share_max) -> uint32_t
	{
		uint32_t const clear = share_max - share_sum;
		uint32_t const a = (share_sum * 255 + (back >> 24) * clear) / share_max;
		uint32_t const r = (share_sum * ((src >> 16) & 0xff) + ((back >> 16) & 0xff) * clear) / share_max;
		uint32_t const g = (share_sum * ((src >> 8) & 0xff) + ((back >> 8) & 0xff) * clear) / share_max;
		uint32_t const b = (share_sum * (src & 0xff) + (back & 0xff) * clear) / share_max;
		return (a << 24) | (r << 16) | (g << 8) | b;
	};

	if (m_output_scaled)
	{
		// The screen has one pixel per sample
		for (int j = cliprect.min_y; j <= cliprect.max_y; j++)
		{
			uint32_t *const bmp = &bitmap.pix(j);
			uint32_t const *const src = &page.pix(j - cliprect.min_y, 0);

			for (int i = cliprect.min_x; i <= cliprect.max_x; i++)
			{
				uint32_t const p = src[i - cliprect.min_x];
				if (p & 0xff000000)
				{
					if (share && (p >> 24) != 0xff)
						bmp[i] = blend(p, bmp[i], p >> 24, 255);
					else
						bmp[i] = p;
				}
			}
		}
		return;
	}

	for (int j = cliprect.min_y; j <= cliprect.max_y; j++)
	{
		uint32_t *const bmp = &bitmap.pix(j);
		int const src_y = (j - cliprect.min_y) * ss;

		for (int i = cliprect.min_x; i <= cliprect.max_x; i++)
		{
			int const src_x = (i - cliprect.min_x) * ss;

			if (ss == 1)
			{
				uint32_t const src = page.pix(src_y, src_x);
				if (src & 0xff000000)
				{
					if (share && (src >> 24) != 0xff)
						bmp[i] = blend(src, bmp[i], src >> 24, 255);
					else
						bmp[i] = src;
				}
				continue;
			}

			if (share)
			{
				// Each page pixel counts for its covered share, 255 being all of it
				uint32_t weight = 0, r = 0, g = 0, b = 0;
				for (int sy = 0; sy < ss; sy++)
				{
					uint32_t const *const src = &page.pix(src_y + sy, src_x);
					for (int sx = 0; sx < ss; sx++)
					{
						uint32_t const p = src[sx];
						uint32_t const w = p >> 24;
						weight += w;
						r += w * ((p >> 16) & 0xff);
						g += w * ((p >> 8) & 0xff);
						b += w * (p & 0xff);
					}
				}

				if (weight == 0)
					continue;

				// colour of the drawn samples, then blended by the share of the output pixel they cover
				uint32_t const drawn = (0xff << 24) | ((r / weight) << 16) | ((g / weight) << 8) | (b / weight);
				bmp[i] = (weight == uint32_t(samples) * 255) ? drawn : blend(drawn, bmp[i], weight, samples * 255);
				continue;
			}

			// Average the samples the 3D layer drew. Where only some were drawn (a polygon edge
			// or a cut-out in a texture), the average is blended over what is behind the layer
			// by that share, which anti-aliases the edge.
			int covered = 0;
			uint32_t a = 0, r = 0, g = 0, b = 0;
			for (int sy = 0; sy < ss; sy++)
			{
				uint32_t const *const src = &page.pix(src_y + sy, src_x);
				for (int sx = 0; sx < ss; sx++)
				{
					uint32_t const p = src[sx];
					if (p & 0xff000000)
					{
						covered++;
						a += p >> 24;
						r += (p >> 16) & 0xff;
						g += (p >> 8) & 0xff;
						b += p & 0xff;
					}
				}
			}

			if (covered == 0)
				continue;

			if (covered < samples)
			{
				// Colour is the average of the drawn samples, weighted by how many were drawn
				uint32_t const back = bmp[i];
				uint32_t const clear = samples - covered;
				a = (a + (back >> 24) * clear) / samples;
				r = (r + ((back >> 16) & 0xff) * clear) / samples;
				g = (g + ((back >> 8) & 0xff) * clear) / samples;
				b = (b + (back & 0xff) * clear) / samples;
			}
			else
			{
				a /= samples;
				r /= samples;
				g /= samples;
				b /= samples;
			}

			bmp[i] = (a << 24) | (r << 16) | (g << 8) | b;
		}
	}
}



DEFINE_DEVICE_TYPE(K001005, k001005_device, "k001005", "Konami 001005 Polygon Renderer")

k001005_device::k001005_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock)
	: device_t(mconfig, K001005, tag, owner, clock)
	, device_video_interface(mconfig, *this)
	, m_k001006(*this, finder_base::DUMMY_TAG)
	, m_fifo(nullptr)
	, m_status(0)
	, m_ram_ptr(0)
	, m_fifo_read_ptr(0)
	, m_fifo_write_ptr(0)
{
	m_ram[0] = nullptr;
	m_ram[1] = nullptr;
}

//-------------------------------------------------
//  device_start - device-specific startup
//-------------------------------------------------

void k001005_device::device_start()
{
	m_ram[0] = std::make_unique<uint16_t[]>(0x140000);
	m_ram[1] = std::make_unique<uint16_t[]>(0x140000);

	m_fifo = std::make_unique<uint32_t[]>(0x800);

	m_renderer = std::make_unique<k001005_renderer>(*this, screen(), m_k001006);

	// The 2D layers draw at native size, so the scaled screen gets them enlarged from a native-size bitmap
	m_output_scale = m_renderer->output_scaled() ? m_renderer->supersample() : 1;
	if (m_output_scale > 1)
	{
		rectangle const visarea = screen().visible_area();
		m_layer.allocate(screen().width(), screen().height());
		screen().configure(screen().width() * m_output_scale, screen().height() * m_output_scale,
				rectangle(visarea.min_x * m_output_scale, (visarea.max_x + 1) * m_output_scale - 1,
					visarea.min_y * m_output_scale, (visarea.max_y + 1) * m_output_scale - 1),
				screen().frame_period());
	}

	save_pointer(NAME(m_ram[0]), 0x140000);
	save_pointer(NAME(m_ram[1]), 0x140000);
	save_pointer(NAME(m_fifo), 0x800);
	save_item(NAME(m_status));
	save_item(NAME(m_ram_ptr));
	save_item(NAME(m_fifo_read_ptr));
	save_item(NAME(m_fifo_write_ptr));
}

//-------------------------------------------------
//  device_reset - device-specific reset
//-------------------------------------------------

void k001005_device::device_reset()
{
	m_status = 0;
	m_ram_ptr = 0;
	m_fifo_read_ptr = 0;
	m_fifo_write_ptr = 0;

	m_renderer->reset();
}

//-------------------------------------------------
//  device_stop - device-specific stop
//-------------------------------------------------

void k001005_device::device_stop()
{
}


/*****************************************************************************
    DEVICE HANDLERS
*****************************************************************************/

void k001005_device::enlarge_layer(bitmap_rgb32 &bitmap, const rectangle &cliprect) const
{
	int const os = m_output_scale;
	for (int y = cliprect.min_y; y <= cliprect.max_y; y++)
	{
		uint32_t const *const src = &m_layer.pix(y / os, 0);
		uint32_t *const dst = &bitmap.pix(y);
		for (int x = cliprect.min_x; x <= cliprect.max_x; x++)
		{
			uint32_t const p = src[x / os];
			if (p & 0xff000000)
				dst[x] = p;
		}
	}
}

void k001005_device::swap_buffers( )
{
	m_renderer->swap_buffers();
}

uint32_t k001005_device::read(address_space &space, offs_t offset, uint32_t mem_mask)
{
	adsp21062_device *dsp = downcast<adsp21062_device*>(&space.device());

	switch(offset)
	{
		case 0x000:         // FIFO read, high 16 bits
			//osd_printf_debug("FIFO_r0: %08X\n", m_fifo_read_ptr);
			return m_fifo[m_fifo_read_ptr] >> 16;

		case 0x001:         // FIFO read, low 16 bits
			{
				//osd_printf_debug("FIFO_r1: %08X\n", m_fifo_read_ptr);
				uint16_t const value = m_fifo[m_fifo_read_ptr] & 0xffff;

				if (!machine().side_effects_disabled())
				{
					if (m_status != 1 && m_status != 2)
					{
						if (m_fifo_read_ptr < 0x3ff)
							dsp->set_flag_input(1, CLEAR_LINE);
						else
							dsp->set_flag_input(1, ASSERT_LINE);
					}
					else
					{
						dsp->set_flag_input(1, ASSERT_LINE);
					}

					m_fifo_read_ptr++;
					m_fifo_read_ptr &= 0x7ff;
				}
				return value;
			}

		case 0x11b:         // status ?
			return 0x8002;

		case 0x11c:         // slave status ?
			return 0x8000;

		case 0x11f:
			{
				uint32_t ret = 0;
				if (m_ram_ptr >= 0x400000)
					ret = m_ram[1][m_ram_ptr & 0x3fffff];
				else
					ret = m_ram[0][m_ram_ptr & 0x3fffff];
				if (!machine().side_effects_disabled())
					m_ram_ptr++;
				return ret;
			}

		default:
			//osd_printf_debug("%s m_r: %08X, %08X\n", machine().describe_context(), offset, mem_mask);
			break;
	}
	return 0;
}

void k001005_device::write(address_space &space, offs_t offset, uint32_t data, uint32_t mem_mask)
{
	adsp21062_device *dsp = downcast<adsp21062_device*>(&space.device());

	switch (offset)
	{
		case 0x000:         // FIFO write
			//osd_printf_debug("%s K001005 FIFO write: %08X\n", machine().describe_context(), data);
			if (m_status != 1 && m_status != 2)
			{
				if (m_fifo_write_ptr < 0x400)
					dsp->set_flag_input(1, ASSERT_LINE);
				else
					dsp->set_flag_input(1, CLEAR_LINE);
			}
			else
			{
				dsp->set_flag_input(1, ASSERT_LINE);
			}

			//osd_printf_debug("%s K001005 FIFO write: %08X\n", machine().describe_context(), data);
			m_fifo[m_fifo_write_ptr] = data;
			m_fifo_write_ptr++;
			m_fifo_write_ptr &= 0x7ff;

			m_renderer->push_data(data);

			// !!! HACK to get past the FIFO B test (GTI Club & Thunder Hurricane) !!!
			if (dsp->pc() == 0x201ee)
			{
				// This is used to make the SHARC timeout
				dsp->spin_until_trigger(10000);
			}
			// !!! HACK to get past the FIFO B test (Winding Heat & Midnight Run) !!!
			if (dsp->pc() == 0x201e6)
			{
				// This is used to make the SHARC timeout
				dsp->spin_until_trigger(10000);
			}
			break;

		case 0x100:     break;

		case 0x101:     break;      // framebuffer width?
		case 0x102:     break;      // framebuffer height?

		case 0x103:     m_renderer->m_viewport_min_x = data & 0xffff; break;
		case 0x104:     m_renderer->m_viewport_max_x = data & 0xffff; break;
		case 0x105:     m_renderer->m_viewport_max_y = data & 0xffff; break;
		case 0x106:     m_renderer->m_viewport_min_y = data & 0xffff; break;

		case 0x107:     m_renderer->m_viewport_center_x = data & 0xffff; break;
		case 0x108:     m_renderer->m_viewport_center_y = data & 0xffff; break;

		case 0x109:                 // far Z value
			// the SHARC code throws away the bottom 11 bits of mantissa and the top 5 bits (to fit in a 16-bit register?)
			m_renderer->m_far_z = u2f((data & 0xffff) << 11);
			break;

		case 0x10a:     m_renderer->m_light_r = data & 0xff; break;
		case 0x10b:     m_renderer->m_light_g = data & 0xff; break;
		case 0x10c:     m_renderer->m_light_b = data & 0xff; break;

		case 0x10d:     m_renderer->m_ambient_r = data & 0xff; break;
		case 0x10e:     m_renderer->m_ambient_g = data & 0xff; break;
		case 0x10f:     m_renderer->m_ambient_b = data & 0xff; break;

		case 0x110:     m_renderer->m_fog_r = data & 0xff; break;
		case 0x111:     m_renderer->m_fog_g = data & 0xff; break;
		case 0x112:     m_renderer->m_fog_b = data & 0xff; break;

		case 0x117:                 // linear fog start Z
			// 4 bits exponent + 12 bits mantissa, similar to far Z value
			// value of 0xffff is used to effectively turn off fog

			// reconstruct float from 16-bit data
			// assuming implicit exponent 1001xxxx, sign bit 0 (z-values are all positive)
			m_renderer->m_reg_fog_start = data & 0xffff;
			m_renderer->m_fog_start_z = u2f((0x90000 | (data & 0xffff)) << 11);
			break;

		case 0x118:                 // linear fog end Z
			// 4 bits exponent + 12 bits mantissa, similar to far Z value
			m_renderer->m_fog_end_z = u2f((0x90000 | (data & 0xffff)) << 11);
			break;

		case 0x119:                 // 1 / (end_fog - start_fog) ?
			// 5 bits exponent + 11 bits mantissa
			break;


		case 0x11a:
			m_status = data;
			m_fifo_write_ptr = 0;
			m_fifo_read_ptr = 0;

			if (data == 2)
			{
				if (m_renderer->fifo_filled())
					m_renderer->render_polygons();

				m_renderer->swap_buffers();
			}
			break;

		case 0x11d:
			m_fifo_write_ptr = 0;
			m_fifo_read_ptr = 0;
			break;

		case 0x11e:
			m_ram_ptr = data;
			break;

		case 0x11f:
			if (m_ram_ptr >= 0x400000)
				m_ram[1][(m_ram_ptr++) & 0x3fffff] = data & 0xffff;
			else
				m_ram[0][(m_ram_ptr++) & 0x3fffff] = data & 0xffff;
			break;

		default:
			//osd_printf_debug("%s m_w: %08X, %08X, %08X\n", machine().describe_context(), data, offset, mem_mask);
			break;
	}

}

void k001005_device::draw(bitmap_rgb32 &bitmap, const rectangle &cliprect)
{
	m_renderer->draw(bitmap, cliprect);
}
