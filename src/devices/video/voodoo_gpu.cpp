// license:BSD-3-Clause
// copyright-holders:Aaron Giles
/***************************************************************************

    voodoo_gpu.cpp

    Frame batch for the host-GPU path of the 3dfx Voodoo renderer.

***************************************************************************/

#include "emu.h"
#include "voodoo.h"


namespace voodoo
{

//-------------------------------------------------
//  add_state - append the register state of a
//  polygon and return its index
//-------------------------------------------------

u32 gpu_batch::add_state(poly_data const &poly, u32 flags)
{
	u32 const index = m_state.size() / STATE_WORDS;
	m_state.resize(m_state.size() + STATE_WORDS);
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
	s[S_BUFFER] = poly.raster.fbzmode().draw_buffer();
	s[S_TEX0] = ~u32(0);
	s[S_TEX1] = ~u32(0);
	s[S_FLAGS] = flags;
	return index;
}


//-------------------------------------------------
//  add_triangle - capture a triangle; x/y are the
//  three corners in setup (upper-left origin)
//  pixels
//-------------------------------------------------

bool gpu_batch::add_triangle(poly_data const &poly, bool y_origin, s32 yorigin, float const *x, float const *y)
{
	if (m_triangles >= MAX_TRIANGLES)
	{
		m_dropped++;
		return false;
	}

	bool const tex0 = poly.tex0 != nullptr;
	bool const tex1 = poly.tex1 != nullptr;
	u32 const prim = add_state(poly, 0);
	m_commands.push_back({ gpu_command::type::TRIANGLE, u32(m_vertices.size()), prim });

	float const ax = float(poly.ax >> 4);
	float const ay = float(poly.ay >> 4);
	for (int i = 0; i < 3; i++)
	{
		gpu_vertex &vert = m_vertices.emplace_back();
		float const dx = x[i] - ax;
		float const dy = y[i] - ay;
		vert.x = x[i];
		vert.y = y_origin ? float(yorigin) - y[i] : y[i];
		vert.z = 0.0f;
		vert.w = 1.0f;
		vert.prim = prim;

		// 12.12 colour deltas and start values, in 0-255 units
		auto const colour = [&] (s32 start, s32 ddx, s32 ddy) { return (double(start) + dy * ddy + dx * ddx) * (1.0 / 4096.0); };
		vert.v[V_R] = float(colour(poly.startr, poly.drdx, poly.drdy));
		vert.v[V_G] = float(colour(poly.startg, poly.dgdx, poly.dgdy));
		vert.v[V_B] = float(colour(poly.startb, poly.dbdx, poly.dbdy));
		vert.v[V_A] = float(colour(poly.starta, poly.dadx, poly.dady));
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
		vert.v[V_UNUSED] = 0.0f;
	}
	m_triangles++;
	return true;
}


//-------------------------------------------------
//  add_fastfill - capture a fast fill of the
//  clip rectangle
//-------------------------------------------------

bool gpu_batch::add_fastfill(poly_data const &poly)
{
	u32 const prim = add_state(poly, 1);
	m_commands.push_back({ gpu_command::type::FASTFILL, 0, prim });
	m_fastfills++;
	return true;
}


//-------------------------------------------------
//  reset - start a new batch
//-------------------------------------------------

void gpu_batch::reset()
{
	m_vertices.clear();
	m_state.clear();
	m_commands.clear();
	m_triangles = 0;
	m_fastfills = 0;
	m_dropped = 0;
}

} // namespace voodoo
