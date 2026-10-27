// license:BSD-3-Clause
// copyright-holders:TrickoUK
/***************************************************************************

    voodoo_gpu.cpp

    Host-GPU path of the 3dfx Voodoo renderer.

***************************************************************************/

#include "emu.h"
#include "voodoo.h"
#include "voodoo_shader.h"

#include "osdepend.h"

#include <chrono>
#include <cmath>
#include <cstring>


namespace voodoo
{

//**************************************************************************
//  GPU BATCH
//**************************************************************************

//-------------------------------------------------
//  lut_row - return the row of the palette pool
//  holding this 256-entry table, adding it unless
//  it matches the last table the TMU used
//-------------------------------------------------

u32 gpu_batch::lut_row(int tmu, rgb_t const *table)
{
	if (table == nullptr)
		return 0;
	if (m_lut_last[tmu] != ~0U && !memcmp(&m_lut[m_lut_last[tmu] * 256], table, 256 * sizeof(u32)))
		return m_lut_last[tmu];
	if (m_lut_rows >= LUT_ROWS)
		return m_lut_last[tmu] != ~0U ? m_lut_last[tmu] : 0;
	memcpy(&m_lut[m_lut_rows * 256], table, 256 * sizeof(u32));
	m_lut_last[tmu] = m_lut_rows;
	return m_lut_rows++;
}


//-------------------------------------------------
//  fog_row - return the row of the fog pool
//  holding the current 64-entry table
//-------------------------------------------------

u32 gpu_batch::fog_row(u8 const *blend, u8 const *delta)
{
	u32 entries[64];
	for (int i = 0; i < 64; i++)
		entries[i] = delta[i] | (u32(blend[i]) << 8);
	if (m_fog_rows != 0 && !memcmp(&m_fog[(m_fog_rows - 1) * 64], entries, sizeof(entries)))
		return m_fog_rows - 1;
	if (m_fog_rows >= FOG_ROWS)
		return m_fog_rows - 1;
	memcpy(&m_fog[m_fog_rows * 64], entries, sizeof(entries));
	return m_fog_rows++;
}


//-------------------------------------------------
//  fill_tmu - decoded texture state of one TMU
//-------------------------------------------------

void gpu_batch::fill_tmu(u32 *block, int tmu, rasterizer_texture const *tex, s64 dsdx, s64 dsdy, s64 dtdx, s64 dtdy)
{
	for (int i = 0; i < 9; i++)
		block[TB_LODOFF + i] = tex->m_lodoffset[i];
	block[TB_WMASK] = tex->m_wmask;
	block[TB_HMASK] = tex->m_hmask;
	block[TB_LODMIN] = tex->m_lodmin;
	block[TB_LODMAX] = tex->m_lodmax;
	block[TB_LODBIAS] = tex->m_lodbias;
	block[TB_LODMASK] = tex->m_lodmask;
	block[TB_DETMAX] = tex->m_detailmax;
	block[TB_DETBIAS] = tex->m_detailbias;
	block[TB_DETSCALE] = tex->m_detailscale;
	block[TB_MASK] = tex->m_mask;
	block[TB_LUTROW] = lut_row(tmu, tex->m_lookup);

	// base LOD from the largest texel step per pixel, as a 24.8 log2
	double const texdx = double(dsdx) * double(dsdx) + double(dtdx) * double(dtdx);
	double const texdy = double(dsdy) * double(dsdy) + double(dtdy) * double(dtdy);
	double const maxval = std::max(texdx, texdy);
	block[TB_LODBASE] = (maxval > 0.0) ? (s32(std::floor((std::log2(maxval) - 64.0) * 256.0)) / 2) : 0;

	// texture RAM range read by the batch
	u32 lo = ~0U, hi = 0;
	for (int i = 0; i < 9; i++)
	{
		lo = std::min(lo, u32(tex->m_lodoffset[i]));
		hi = std::max(hi, u32(tex->m_lodoffset[i]));
	}
	hi += (u32(tex->m_wmask) + 1) * (u32(tex->m_hmask) + 1) * 2;
	m_tex_mask[tmu] = tex->m_mask;
	if (hi > tex->m_mask + 1)
	{
		lo = 0;
		hi = tex->m_mask + 1;
	}
	m_tex_lo[tmu] = std::min(m_tex_lo[tmu], lo);
	m_tex_hi[tmu] = std::max(m_tex_hi[tmu], hi);
}


//-------------------------------------------------
//  texture_hazard - does a texture RAM write hit
//  memory the batch has read?
//-------------------------------------------------

bool gpu_batch::texture_hazard(int tmu, u32 offs, u32 bytes) const
{
	return m_tex_lo[tmu] < m_tex_hi[tmu] && offs < m_tex_hi[tmu] && offs + bytes > m_tex_lo[tmu];
}


//-------------------------------------------------
//  add_state - append the register state shared
//  by every primitive kind
//-------------------------------------------------

u32 gpu_batch::add_state(poly_data const &poly, u32 flags, int buffer)
{
	u32 const index = m_state.size() / STATE_WORDS;
	m_state.resize(m_state.size() + STATE_WORDS, 0);
	u32 *const s = &m_state[index * STATE_WORDS];

	s[S_FBZCOLORPATH] = poly.raster.fbzcp().raw();
	s[S_FBZMODE] = poly.raster.fbzmode().raw();
	s[S_ALPHAMODE] = poly.raster.alphamode().raw();
	s[S_FOGMODE] = poly.raster.fogmode().raw();
	s[S_TEXMODE0] = poly.raster.texmode0().raw();
	s[S_TEXMODE1] = poly.raster.texmode1().raw();
	s[S_COLOR0] = poly.color0;
	s[S_COLOR1] = poly.color1;
	s[S_CHROMAKEY] = poly.chromakey;
	s[S_FOGCOLOR] = poly.fogcolor;
	s[S_ZACOLOR] = poly.zacolor;
	s[S_STIPPLE] = poly.stipple;
	s[S_ALPHAREF] = poly.alpharef;
	s[S_CLIP_LR] = poly.clipleft | (u32(poly.clipright) << 16);
	s[S_CLIP_TB] = poly.cliptop | (u32(poly.clipbottom) << 16);
	s[S_BUFFER] = buffer;
	s[S_FLAGS] = flags;
	return index;
}


//-------------------------------------------------
//  add_triangle - capture a triangle; x/y are the
//  three corners in setup (upper-left origin)
//  pixels
//-------------------------------------------------

char const *gpu_batch::check(poly_data const &poly)
{
	auto const fbzmode = poly.raster.fbzmode();
	auto const alphamode = poly.raster.alphamode();
	if (fbzmode.enable_alpha_planes())
		return "alpha planes";
	if (fbzmode.enable_depthbuf() && fbzmode.depth_source_compare())
		return "depth source compare";
	if (fbzmode.enable_stipple() && fbzmode.stipple_pattern() == 0)
		return "rotating stipple";
	if (alphamode.alphablend() && alphamode.dstrgbblend() == 15)
		return "colour-before-fog blend";
	return nullptr;
}


void gpu_batch::add_triangle(poly_data const &poly, int buffer, bool y_origin, s32 yorigin, float const *x, float const *y, globals const &g)
{
	close_stamp();
	auto const fbzmode = poly.raster.fbzmode();
	auto const alphamode = poly.raster.alphamode();
	if (!m_unsupported)
	{
		if (fbzmode.enable_alpha_planes())
			m_unsupported = "alpha planes";
		else if (fbzmode.enable_depthbuf() && fbzmode.depth_source_compare())
			m_unsupported = "depth source compare";
		else if (fbzmode.enable_stipple() && fbzmode.stipple_pattern() == 0)
			m_unsupported = "rotating stipple";
		else if (alphamode.alphablend() && alphamode.dstrgbblend() == 15)
			m_unsupported = "colour-before-fog blend";
	}

	u32 const prim = add_state(poly, 0, buffer);
	u32 *const s = &m_state[prim * STATE_WORDS];
	s[S_CHROMARANGE] = g.chroma_range;
	s[S_FOGMASK] = g.fogdelta_mask;
	s[S_BILINMASK] = g.bilinear_mask;
	s[S_FOGROW] = 0;
	auto const fogmode = poly.raster.fogmode();
	if (fogmode.enable_fog() && !fogmode.fog_constant() && fogmode.fog_zalpha() == 0)
		s[S_FOGROW] = fog_row(g.fogblend, g.fogdelta);
	u32 const generic = poly.raster.generic();
	bool const tex0 = (generic & rasterizer_params::GENERIC_TEX0) && poly.tex0 != nullptr;
	bool const tex1 = (generic & rasterizer_params::GENERIC_TEX1) && poly.tex1 != nullptr;
	s[S_FLAGS] = ((generic & rasterizer_params::GENERIC_TEX0_IDENTITY) ? 4 : 0) | ((generic & rasterizer_params::GENERIC_TEX1_IDENTITY) ? 8 : 0);
	if (tex0)
		fill_tmu(&s[S_TMU0], 0, poly.tex0, poly.ds0dx, poly.ds0dy, poly.dt0dx, poly.dt0dy);
	if (tex1)
		fill_tmu(&s[S_TMU1], 1, poly.tex1, poly.ds1dx, poly.ds1dy, poly.dt1dx, poly.dt1dy);
	// the normalised texture mode folds RGB565, ARGB1555 and ARGB4444 into one format code; the
	// texel table the texture was set up with says which one it is
	auto const fix_format = [&g] (u32 &word, rasterizer_texture const *tex)
	{
		if (reg_texture_mode(word).format() != 10)
			return;
		u32 fmt = 10;
		if (tex->m_lookup == g.argb1555)
			fmt = 11;
		else if (tex->m_lookup == g.argb4444)
			fmt = 12;
		word = (word & ~(0xfU << 8)) | (fmt << 8);
	};
	if (tex0)
		fix_format(s[S_TEXMODE0], poly.tex0);
	if (tex1)
		fix_format(s[S_TEXMODE1], poly.tex1);
	if (!tex0)
		s[S_TEXMODE0] = 0xffffffff;
	if (!tex1)
		s[S_TEXMODE1] = 0xffffffff;

	m_commands.push_back({ gpu_command::type::TRIANGLE, u32(m_vertices.size()), prim });

	float const ax = float(poly.ax >> 4);
	float const ay = float(poly.ay >> 4);
	for (int i = 0; i < 3; i++)
	{
		gpu_vertex &vert = m_vertices.emplace_back();
		double const dx = double(x[i]) - 0.5 - ax;
		double const dy = double(y[i]) - 0.5 - ay;
		vert.x = x[i];
		vert.y = y_origin ? float(yorigin + 1) - y[i] : y[i];
		vert.z = 0.5f;
		vert.w = 1.0f;
		vert.prim = prim;

		auto const colour = [&] (s32 start, s32 ddx, s32 ddy) { return float((double(start) + dy * ddy + dx * ddx) * (1.0 / 4096.0)); };
		vert.v[V_R] = colour(poly.startr, poly.drdx, poly.drdy);
		vert.v[V_G] = colour(poly.startg, poly.dgdx, poly.dgdy);
		vert.v[V_B] = colour(poly.startb, poly.dbdx, poly.dbdy);
		vert.v[V_A] = colour(poly.starta, poly.dadx, poly.dady);
		vert.v[V_Z] = float(double(poly.startz) + dy * poly.dzdy + dx * poly.dzdx);
		vert.v[V_W] = float(double(poly.startw) + dy * double(poly.dwdy) + dx * double(poly.dwdx));
		if (tex0)
		{
			vert.v[V_S0] = float(double(poly.starts0) + dy * double(poly.ds0dy) + dx * double(poly.ds0dx));
			vert.v[V_T0] = float(double(poly.startt0) + dy * double(poly.dt0dy) + dx * double(poly.dt0dx));
			vert.v[V_W0] = float(double(poly.startw0) + dy * double(poly.dw0dy) + dx * double(poly.dw0dx));
		}
		else
			vert.v[V_S0] = vert.v[V_T0] = vert.v[V_W0] = 0.0f;
		if (tex1)
		{
			vert.v[V_S1] = float(double(poly.starts1) + dy * double(poly.ds1dy) + dx * double(poly.ds1dx));
			vert.v[V_T1] = float(double(poly.startt1) + dy * double(poly.dt1dy) + dx * double(poly.dt1dx));
			vert.v[V_W1] = float(double(poly.startw1) + dy * double(poly.dw1dy) + dx * double(poly.dw1dx));
		}
		else
			vert.v[V_S1] = vert.v[V_T1] = vert.v[V_W1] = 0.0f;
	}
	m_triangles++;
}


//-------------------------------------------------
//  add_fastfill - capture a fast fill of the clip
//  rectangle; the rows are those the scanline
//  fill writes
//-------------------------------------------------

void gpu_batch::add_fastfill(poly_data const &poly, int buffer, bool y_origin, s32 yorigin)
{
	close_stamp();
	u32 const prim = add_state(poly, 1, buffer);
	u32 *const s = &m_state[prim * STATE_WORDS];
	s32 top = poly.cliptop, bottom = poly.clipbottom;
	if (y_origin)
	{
		top = yorigin - (poly.clipbottom - 1);
		bottom = yorigin - poly.cliptop + 1;
	}
	s[S_CLIP_TB] = u32(std::max(top, 0)) | (u32(std::max(bottom, 0)) << 16);
	s[S_CHROMARANGE] = yorigin;                 // fast fills dither by the unflipped row
	m_commands.push_back({ gpu_command::type::FASTFILL, 0, prim });
	m_fastfills++;
}


//-------------------------------------------------
//  set_geometry - size of one colour buffer
//-------------------------------------------------

void gpu_batch::set_geometry(u32 rowpixels, u32 rows)
{
	close_stamp();
	m_stage_rowpixels = rowpixels;
	m_stage_rows = rows;
	m_stage.assign(size_t(rowpixels) * rows, 0);
	m_stage_z.clear();
	m_stage_has_z = false;
}


//-------------------------------------------------
//  add_lfb_pixel - record a pixel written through
//  the linear frame buffer; the stamp stays open
//  until the next triangle or fast fill
//-------------------------------------------------

void gpu_batch::add_lfb_depth(int buffer, int x, int y, u16 depth)
{
	if (x < 0 || y < 0 || u32(x) >= m_stage_rowpixels || u32(y) >= m_stage_rows)
		return;
	if (m_stage_buffer != buffer)
		close_stamp();
	if (m_stage_z.empty())
		m_stage_z.assign(m_stage.size(), 0);
	if (m_stage_x1 < m_stage_x0)
	{
		m_stage_buffer = buffer;
		m_stage_x0 = m_stage_x1 = x;
		m_stage_y0 = m_stage_y1 = y;
	}
	m_stage_x0 = std::min(m_stage_x0, x);
	m_stage_x1 = std::max(m_stage_x1, x);
	m_stage_y0 = std::min(m_stage_y0, y);
	m_stage_y1 = std::max(m_stage_y1, y);
	m_stage_z[size_t(y) * m_stage_rowpixels + x] = 0x10000 | depth;
	m_stage_has_z = true;
}

void gpu_batch::add_lfb_pixel(int buffer, int x, int y, u32 argb)
{
	if (x < 0 || y < 0 || u32(x) >= m_stage_rowpixels || u32(y) >= m_stage_rows)
		return;
	if (m_stage_buffer != buffer)
		close_stamp();
	if (m_stage_x1 < m_stage_x0)
	{
		m_stage_buffer = buffer;
		m_stage_x0 = m_stage_x1 = x;
		m_stage_y0 = m_stage_y1 = y;
	}
	m_stage_x0 = std::min(m_stage_x0, x);
	m_stage_x1 = std::max(m_stage_x1, x);
	m_stage_y0 = std::min(m_stage_y0, y);
	m_stage_y1 = std::max(m_stage_y1, y);
	m_stage[size_t(y) * m_stage_rowpixels + x] = argb;
}


//-------------------------------------------------
//  close_stamp - turn the open pixels into a
//  stamp command
//-------------------------------------------------

void gpu_batch::close_stamp()
{
	if (m_stage_x1 < m_stage_x0)
		return;
	stamp &s = m_stamps.emplace_back();
	s.buffer = m_stage_buffer;
	s.x = m_stage_x0;
	s.y = m_stage_y0;
	s.w = m_stage_x1 - m_stage_x0 + 1;
	s.h = m_stage_y1 - m_stage_y0 + 1;
	s.pixels.resize(size_t(s.w) * s.h);
	if (m_stage_has_z)
		s.depth.resize(size_t(s.w) * s.h);
	for (int row = 0; row < s.h; row++)
	{
		u32 *src = &m_stage[size_t(s.y + row) * m_stage_rowpixels + s.x];
		std::copy_n(src, s.w, &s.pixels[size_t(row) * s.w]);
		std::fill_n(src, s.w, 0);
		if (m_stage_has_z)
		{
			u32 *srcz = &m_stage_z[size_t(s.y + row) * m_stage_rowpixels + s.x];
			std::copy_n(srcz, s.w, &s.depth[size_t(row) * s.w]);
			std::fill_n(srcz, s.w, 0);
		}
	}
	m_stage_has_z = false;
	u32 const prim = m_state.size() / STATE_WORDS;
	m_state.resize(m_state.size() + STATE_WORDS, 0);
	u32 *const st = &m_state[prim * STATE_WORDS];
	st[S_BUFFER] = s.buffer;
	st[S_FLAGS] = 2;
	st[S_CLIP_LR] = u32(s.x) | (u32(s.x + s.w) << 16);
	st[S_CLIP_TB] = u32(s.y) | (u32(s.y + s.h) << 16);
	m_commands.push_back({ gpu_command::type::STAMP, u32(m_stamps.size() - 1), prim });
	m_stage_x0 = m_stage_y0 = 0;
	m_stage_x1 = m_stage_y1 = -1;
	m_stage_buffer = -1;
}


//-------------------------------------------------
//  reset - start a new batch
//-------------------------------------------------

void gpu_batch::reset()
{
	m_vertices.clear();
	m_state.clear();
	m_commands.clear();
	m_stamps.clear();
	m_lut_rows = 0;
	m_fog_rows = 0;
	m_lut_last[0] = m_lut_last[1] = ~0U;
	m_tex_lo[0] = m_tex_lo[1] = ~0U;
	m_tex_hi[0] = m_tex_hi[1] = 0;
	m_triangles = 0;
	m_fastfills = 0;
}


//**************************************************************************
//  GPU RENDERER
//**************************************************************************

gpu_renderer::gpu_renderer(running_machine &machine, osd::gpu_engine *engine) :
	m_machine(machine),
	m_engine(engine)
{
}

gpu_renderer::~gpu_renderer()
{
	if (m_engine)
	{
		for (auto tex : { m_tmu_tex[0], m_shared_ram ? osd::GPU_NULL_HANDLE : m_tmu_tex[1], m_lut_tex, m_fog_tex, m_stamp_tex, m_stampz_tex })
			if (tex)
				m_engine->destroy_texture(tex);
		if (m_module)
			m_engine->destroy_shader_module(m_module);
	}
}


//-------------------------------------------------
//  init - create the shader module and the
//  texture objects
//-------------------------------------------------

bool gpu_renderer::init(u8 *tmu0_ram, u32 tmu0_size, u8 *tmu1_ram, u32 tmu1_size)
{
	m_ram[0] = tmu0_ram;
	m_ram_size[0] = tmu0_size;
	m_ram[1] = tmu1_ram;
	m_ram_size[1] = tmu1_size;

	osd::gpu_shader_module module;
	module.name = "voodoo";
	module.fragment_source = VOODOO_FRAGMENT_SOURCE;
	if (m_machine.osd().msaa() > 0)
		module.centroid_mask = 0xfff;
	module.varying_count = gpu_vertex::VARYINGS;
	for (int i = 0; i < gpu_vertex::VARYINGS; i++)
		module.varying_mode[i] = osd::gpu_varying_mode::NOPERSPECTIVE;
	module.state_stride = STATE_WORDS;
	module.writes_depth = true;
	module.binding_count = 6;
	module.binding_name[0] = "tmu0";
	module.binding_format[0] = osd::gpu_texture_format::R8UI;
	module.binding_name[1] = "tmu1";
	module.binding_format[1] = osd::gpu_texture_format::R8UI;
	module.binding_name[2] = "lut";
	module.binding_format[2] = osd::gpu_texture_format::R32UI;
	module.binding_name[3] = "fog";
	module.binding_format[3] = osd::gpu_texture_format::R32UI;
	module.binding_name[4] = "stamp";
	module.binding_format[4] = osd::gpu_texture_format::R32UI;
	module.binding_name[5] = "stampz";
	module.binding_format[5] = osd::gpu_texture_format::R32UI;
	m_module = m_engine->create_shader_module(module);

	osd::gpu_texture_desc desc;
	desc.format = osd::gpu_texture_format::R8UI;
	desc.width = 2048;
	m_shared_ram = m_ram[1] == m_ram[0];
	for (int i = 0; i < 2; i++)
	{
		desc.height = std::max<u32>(m_ram_size[i] / 2048, 1);
		if (i == 1 && m_shared_ram)
		{
			// the TMUs of the Banshee and Voodoo 3 share one memory with the frame buffers
			m_tmu_tex[1] = m_tmu_tex[0];
			m_dirty[1] = std::make_unique<osd::gpu_dirty_region>(2048, desc.height);
			continue;
		}
		m_tmu_tex[i] = m_engine->create_texture(desc);
		m_dirty[i] = std::make_unique<osd::gpu_dirty_region>(2048, desc.height);
	}
	desc.format = osd::gpu_texture_format::R32UI;
	desc.width = 256;
	desc.height = gpu_batch::LUT_ROWS;
	m_lut_tex = m_engine->create_texture(desc);
	desc.width = 64;
	desc.height = gpu_batch::FOG_ROWS;
	m_fog_tex = m_engine->create_texture(desc);
	desc.width = 1024;
	desc.height = 1024;
	m_stamp_tex = m_engine->create_texture(desc);
	m_stampz_tex = m_engine->create_texture(desc);

	m_ok = m_module && m_tmu_tex[0] && m_tmu_tex[1] && m_lut_tex && m_fog_tex && m_stamp_tex && m_stampz_tex;
	return m_ok;
}


//-------------------------------------------------
//  configure - size the target for the colour
//  buffer stack
//-------------------------------------------------

void gpu_renderer::configure(u32 rowpixels, u32 rows, int buffers)
{
	if (rowpixels == m_rowpixels && rows == m_rows && buffers == m_buffers)
		return;
	m_rowpixels = rowpixels;
	m_rows = rows;
	m_buffers = buffers;
	m_scale = std::max(1, m_machine.osd().supersample());
	int const limit = m_engine->caps().max_texture_size;
	// the stack is sized for every slot, so a tall stack at 4x would not fit: 3x is the largest scale that does
	if (int(rows) * buffers * m_scale > limit)
		m_scale = std::min(m_scale, 3);
	while (m_scale > 1 && (int(rowpixels) * m_scale > limit || int(rows) * buffers * m_scale > limit))
		m_scale--;
	m_batch.set_geometry(rowpixels, rows);
	m_generation++;
}


//-------------------------------------------------
//  latch - stop using the GPU path
//-------------------------------------------------

void gpu_renderer::latch(char const *reason)
{
	if (m_latched)
		return;
	osd_printf_info("Voodoo GPU path disabled: %s\n", reason);
	m_latched = true;
	m_batch.reset();
}


//-------------------------------------------------
//  texture_write - texture RAM changed
//-------------------------------------------------

void gpu_renderer::texture_write(int tmu, u32 offs, u32 bytes)
{
	if (!m_ok)
		return;
	if (m_batch.texture_hazard(tmu, offs, bytes) || (m_shared_ram && m_batch.texture_hazard(tmu ^ 1, offs, bytes)))
	{
		why("texture hazard");
		flush();
	}
	if (m_shared_ram)
		tmu = 0;
	u32 const row0 = offs / 2048;
	u32 const row1 = (offs + bytes - 1) / 2048;
	m_dirty[tmu]->mark(0, row0, 2048, row1 - row0 + 1);
}


//-------------------------------------------------
//  invalidate - state load or reset: clear the
//  target and reload every table
//-------------------------------------------------

void gpu_renderer::invalidate()
{
	m_batch.reset();
	m_generation++;
	m_dirty[0]->mark_all();
	if (!m_shared_ram)
		m_dirty[1]->mark_all();
}


//-------------------------------------------------
//  flush - build and submit one engine frame from
//  the batch
//-------------------------------------------------

bool gpu_renderer::profiling()
{
	static bool const enabled = getenv("MAME_VOODOO_GPU_PROF") != nullptr;
	return enabled;
}

void gpu_renderer::profile_report()
{
	if (++m_prof.presents % 120)
		return;
	profile_data &p = m_prof;
	double const n = 120;
	std::string reasons;
	for (auto const &r : p.reasons)
		reasons += util::string_format(" %s=%.1f", r.first, r.second / n);
	std::string const line = util::string_format("Voodoo GPU prof/frame: capture %.2f ms (%.0f tris) | flush build %.2f submit %.2f (upload %.2f draw %.2f gpu %.2f readback %.2f) | present CPU %.2f | "
			"flushes %.1f, tris %.0f, fills %.1f, verts %.0f, states %.0f, draws %.1f, upload %.2f MB, readbacks %.1f |%s\n",
			p.capture_ms / n, double(p.capture_count) / n, p.build_ms / n, p.submit_ms / n, p.upload_ms / n, p.draw_ms / n, p.gpu_ms / n, p.readback_ms / n, p.present_ms / n,
			p.flushes / n, p.triangles / n, p.fills / n, p.vertices / n, p.states / n, p.draws / n, p.uploaded / n / 1048576.0, p.readbacks / n, reasons);
	// a value other than 1 names a file to append to, which survives a killed run
	if (char const *path = getenv("MAME_VOODOO_GPU_PROF"); path && strcmp(path, "1"))
	{
		if (FILE *f = fopen(path, "a"))
		{
			fputs(line.c_str(), f);
			fclose(f);
		}
	}
	else
		osd_printf_info("%s", line);
	p = profile_data();
	p.presents = 120;
}

//-------------------------------------------------
//  batch_writes_buffer - true if any command of the
//  batch can change the colour of a buffer slot
//-------------------------------------------------

bool gpu_renderer::batch_writes_buffer(int buffer) const
{
	for (gpu_command const &cmd : m_batch.commands())
	{
		u32 const *s = &m_batch.state()[cmd.prim * STATE_WORDS];
		if (s[S_BUFFER] == u32(buffer) && (cmd.kind == gpu_command::type::STAMP || reg_fbz_mode(s[S_FBZMODE]).rgb_buffer_mask()))
			return true;
	}
	return false;
}

bool gpu_renderer::flush(int read_buffer, u32 *out, u32 first_row, u32 row_count, bool read_first, u32 out_stride, bool opaque, u8 const *channel_lut, int downsample, u32 col_count)
{
	if (!usable() || m_rowpixels == 0)
		return false;
	bool const prof = profiling();
	auto const t_start = std::chrono::steady_clock::now();
	auto const ms_since = [] (std::chrono::steady_clock::time_point from) { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - from).count(); };
	if (prof)
	{
		m_prof.reasons[m_why ? m_why : "other"]++;
		m_prof.flushes++;
		m_prof.triangles += m_batch.triangle_count();
		m_prof.fills += m_batch.fastfill_count();
		m_prof.vertices += m_batch.vertices().size();
		m_prof.states += m_batch.state().size() / STATE_WORDS;
		if (read_buffer >= 0)
		{
			m_prof.reasons[batch_writes_buffer(read_buffer) ? "front_written" : "front_untouched"]++;
		}
	}
	m_why = nullptr;
	if (m_batch.unsupported())
	{
		latch(m_batch.unsupported());
		return false;
	}
	m_batch.close_stamp();
	int const scale = m_scale;

	// texture RAM and table updates
	std::vector<osd::gpu_texture_update> updates;
	for (int i = 0; i < 2; i++)
	{
		int x, y, w, h;
		if (i == 1 && m_shared_ram)
			continue;
		if (m_ram[i] && m_dirty[i]->take(x, y, w, h))
		{
			osd::gpu_texture_update &u = updates.emplace_back();
			u.texture = m_tmu_tex[i];
			u.x = 0;
			u.y = y;
			u.w = 2048;
			u.h = h;
			u.data = m_ram[i] + y * 2048;
		}
	}
	if (m_batch.lut_rows())
	{
		osd::gpu_texture_update &u = updates.emplace_back();
		u.texture = m_lut_tex;
		u.w = 256;
		u.h = m_batch.lut_rows();
		u.data = m_batch.lut();
	}
	if (m_batch.fog_rows())
	{
		osd::gpu_texture_update &u = updates.emplace_back();
		u.texture = m_fog_tex;
		u.w = 64;
		u.h = m_batch.fog_rows();
		u.data = m_batch.fog();
	}

	// vertices and state; fast fills become two triangles
	std::vector<u32> state(m_batch.state());
	std::vector<gpu_vertex> verts;
	verts.reserve(m_batch.vertices().size() + 6 * m_batch.fastfill_count());
	std::vector<osd::gpu_command> commands;
	for (u32 p = 0; p < state.size() / STATE_WORDS; p++)
	{
		u32 *s = &state[p * STATE_WORDS];
		s[S_BUFROW] = s[S_BUFFER] * m_rows;
		s[S_SCALE] = scale;
	}
	osd::gpu_command run;
	bool have_run = false;
	auto const close_run = [&] ()
	{
		if (have_run && run.vertex_count != 0)
			commands.push_back(run);
		have_run = false;
	};
	for (gpu_command const &cmd : m_batch.commands())
	{
		u32 const *s = &state[cmd.prim * STATE_WORDS];
		auto const fbzmode = reg_fbz_mode(s[S_FBZMODE]);
		auto const alphamode = reg_alpha_mode(s[S_ALPHAMODE]);
		float const bufrow = float(s[S_BUFROW]);
		if (cmd.kind == gpu_command::type::STAMP)
		{
			close_run();
			gpu_batch::stamp const &st = m_batch.stamps()[cmd.first_vertex];
			osd::gpu_texture_update upd;
			upd.texture = m_stamp_tex;
			upd.x = st.x;
			upd.y = st.y;
			upd.w = st.w;
			upd.h = st.h;
			upd.data = st.pixels.data();
			upd.pitch_bytes = st.w * 4;
			commands.push_back(osd::gpu_command::make_update(upd));
			if (!st.depth.empty())
			{
				osd::gpu_texture_update updz = upd;
				updz.texture = m_stampz_tex;
				updz.data = st.depth.data();
				commands.push_back(osd::gpu_command::make_update(updz));
			}
		}
		osd::gpu_command next = osd::gpu_command::make_draw(m_module, u32(verts.size()), 0);
		next.bindings[0] = m_tmu_tex[0];
		next.bindings[1] = m_tmu_tex[1];
		next.bindings[2] = m_lut_tex;
		next.bindings[3] = m_fog_tex;
		next.bindings[4] = m_stamp_tex;
		next.bindings[5] = m_stampz_tex;
		next.color_write = fbzmode.rgb_buffer_mask();
		next.depth_write = fbzmode.aux_buffer_mask();
		if (cmd.kind == gpu_command::type::STAMP)
		{
			next.variant = 2;
			next.color_write = true;
			next.depth_write = false;
			next.depth_func = osd::gpu_depth_func::ALWAYS;
			next.multisample = false;
		}
		else if (cmd.kind == gpu_command::type::FASTFILL)
		{
			next.variant = 1;
			next.depth_func = osd::gpu_depth_func::ALWAYS;
			next.multisample = false;
		}
		else
		{
			static constexpr osd::gpu_depth_func FUNCS[8] = {
				osd::gpu_depth_func::NEVER, osd::gpu_depth_func::LESS, osd::gpu_depth_func::EQUAL, osd::gpu_depth_func::LEQUAL,
				osd::gpu_depth_func::GREATER, osd::gpu_depth_func::NOTEQUAL, osd::gpu_depth_func::GEQUAL, osd::gpu_depth_func::ALWAYS };
			next.depth_func = fbzmode.enable_depthbuf() ? FUNCS[fbzmode.depth_function()] : osd::gpu_depth_func::ALWAYS;
			next.variant = osd::gpu_aniso_variant(m_machine.osd().anisotropy());
			// Only geometry with a real depth compare has its edges multisampled. Everything drawn without one (HUD,
			// text, logos, backdrops) covers whole pixels, so an overlay never blends toward the picture behind it.
			next.multisample = next.depth_func != osd::gpu_depth_func::ALWAYS;
			if (alphamode.alphablend())
			{
				using F = osd::gpu_blend_factor;
				static constexpr F SRC[16] = { F::ZERO, F::SRC_ALPHA, F::DST_COLOR, F::ONE, F::ONE, F::ONE_MINUS_SRC_ALPHA, F::ONE_MINUS_DST_COLOR, F::ZERO,
						F::ZERO, F::ZERO, F::ZERO, F::ZERO, F::ZERO, F::ZERO, F::ZERO, F::ZERO };
				static constexpr F DST[16] = { F::ZERO, F::SRC_ALPHA, F::SRC_COLOR, F::ONE, F::ONE, F::ONE_MINUS_SRC_ALPHA, F::ONE_MINUS_SRC_COLOR, F::ZERO,
						F::ZERO, F::ZERO, F::ZERO, F::ZERO, F::ZERO, F::ZERO, F::ZERO, F::ZERO };
				next.blend.enable = true;
				next.blend.src = alphamode.srcrgbblend() == 15 ? F::ZERO : SRC[alphamode.srcrgbblend()];
				next.blend.dst = DST[alphamode.dstrgbblend()];
			}
		}

		// extend the current draw when the state matches
		if (have_run && run.variant == next.variant && run.color_write == next.color_write && run.depth_write == next.depth_write
				&& run.depth_func == next.depth_func && run.blend.enable == next.blend.enable && run.blend.src == next.blend.src && run.blend.dst == next.blend.dst)
		{
			next = run;
			next.vertex_count = 0;
		}
		else
		{
			close_run();
			run = next;
			have_run = true;
		}

		if (cmd.kind == gpu_command::type::TRIANGLE)
		{
			for (int i = 0; i < 3; i++)
			{
				gpu_vertex v = m_batch.vertices()[cmd.first_vertex + i];
				v.x *= scale;
				v.y = (v.y + bufrow) * scale;
				verts.push_back(v);
			}
			run.vertex_count += 3;
		}
		else
		{
			float const x0 = float(s[S_CLIP_LR] & 0xffff) * scale, x1 = float(s[S_CLIP_LR] >> 16) * scale;
			float const y0 = (float(s[S_CLIP_TB] & 0xffff) + bufrow) * scale, y1 = (float(s[S_CLIP_TB] >> 16) + bufrow) * scale;
			float const px[6] = { x0, x1, x0, x1, x1, x0 }, py[6] = { y0, y0, y1, y0, y1, y1 };
			for (int i = 0; i < 6; i++)
			{
				gpu_vertex &v = verts.emplace_back();
				memset(&v, 0, sizeof(v));
				v.x = px[i];
				v.y = py[i];
				v.z = 0.5f;
				v.w = 1.0f;
				v.prim = cmd.prim;
			}
			run.vertex_count += 6;
			if (cmd.kind == gpu_command::type::STAMP && !m_batch.stamps()[cmd.first_vertex].depth.empty())
			{
				// a second draw writes the depth of the stamp
				osd::gpu_command zrun = run;
				close_run();
				zrun.variant = 3;
				zrun.color_write = false;
				zrun.depth_write = true;
				zrun.first_vertex = verts.size();
				zrun.vertex_count = 6;
				for (int i = 0; i < 6; i++)
				{
					gpu_vertex v = verts[verts.size() - 6];
					verts.push_back(v);
				}
				commands.push_back(zrun);
			}
		}
	}
	close_run();

	osd::gpu_frame frame;
	frame.target.width = m_rowpixels * scale;
	frame.target.height = m_rows * m_buffers * scale;
	frame.target.persistent = true;
	frame.target.depth = osd::gpu_depth_mode::REAL;
	frame.target.msaa_samples = m_machine.osd().msaa();
	frame.target.generation = m_generation;
	frame.updates = updates.data();
	frame.update_count = updates.size();
	frame.vertices = verts.data();
	frame.vertex_count = verts.size();
	frame.varyings_per_vertex = gpu_vertex::VARYINGS;
	frame.prim_state = state.data();
	frame.prim_count = state.size() / STATE_WORDS;
	frame.state_stride = STATE_WORDS;
	frame.commands = commands.data();
	frame.command_count = commands.size();
	osd::gpu_readback_rect readback;
	if (read_buffer >= 0 && out)
	{
		readback.x = 0;
		if (row_count == 0)
			row_count = m_rows;
		readback.y = (read_buffer * m_rows + first_row) * scale;
		readback.w = (col_count ? col_count : m_rowpixels) * scale;
		readback.h = row_count * scale;
		readback.out = out;
		readback.stride = out_stride ? out_stride : readback.w / downsample;
		readback.downsample = downsample;
		readback.opaque = opaque;
		readback.channel_lut = channel_lut;
		if (read_first)
		{
			// the picture is complete before this batch starts, so it is read before the draws and the
			// emulation thread does not wait for them
			commands.insert(commands.begin(), osd::gpu_command::make_readback(readback));
			frame.commands = commands.data();
			frame.command_count = commands.size();
		}
		else
		{
			frame.readbacks = &readback;
			frame.readback_count = 1;
		}
	}
	double const build_ms = prof ? ms_since(t_start) : 0.0;
	auto const t_submit = std::chrono::steady_clock::now();
	bool const ok = m_engine->submit_frame(frame);
	if (prof)
	{
		auto const &st = m_engine->last_frame_stats();
		m_prof.build_ms += build_ms;
		m_prof.submit_ms += ms_since(t_submit);
		m_prof.upload_ms += st.upload_ms;
		m_prof.draw_ms += st.draw_ms;
		m_prof.gpu_ms += st.gpu_ms;
		m_prof.readback_ms += st.readback_ms;
		m_prof.uploaded += st.uploaded_bytes;
		m_prof.draws += st.draw_calls;
		m_prof.readbacks += frame.readback_count;
	}
	m_serial++;
	m_batch.reset();
	return ok;
}

} // namespace voodoo
