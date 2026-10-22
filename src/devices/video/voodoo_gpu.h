// license:BSD-3-Clause
// copyright-holders:Aaron Giles
/***************************************************************************

    voodoo_gpu.h

    Frame batch for the host-GPU path of the 3dfx Voodoo renderer. Triangles
    and fast fills are captured as they reach the rasterizer queue and are
    closed at each buffer swap.

***************************************************************************/

#ifndef MAME_VIDEO_VOODOO_GPU_H
#define MAME_VIDEO_VOODOO_GPU_H

#pragma once

#include "voodoo_regs.h"
#include "voodoo_render.h"

#include <vector>


namespace voodoo
{

// ======================> gpu_vertex

// One triangle corner. The iterated values are the plane equations of the
// triangle evaluated at the corner, relative to the integer pixel the setup
// registers are anchored on (poly_data ax/ay >> 4), which is how the scanline
// rasterizer evaluates them.
struct gpu_vertex
{
	static constexpr int VARYINGS = 13;

	float x, y;                 // screen position in pixels, Y flipped when fbzMode selects the lower-left origin
	float z, w;                 // padding for the engine vertex layout: x,y,z,w
	u32 prim;                   // index of the state record
	float v[VARYINGS];          // see the V_* indices
};

enum : int
{
	V_R, V_G, V_B, V_A,         // iterated colour, 0-255 scale
	V_Z,                        // iterated Z, 20.12
	V_W,                        // iterated W, 16.32
	V_S0, V_T0, V_W0,           // TMU 0 S, T (14.18) and W (2.30)
	V_S1, V_T1, V_W1,           // TMU 1
	V_UNUSED
};


// ======================> gpu_state

// Per-triangle register state selecting the pixel pipeline. Words are raw
// register values so a shader module can decode them with the same bit
// layout as voodoo_regs.h.
enum : int
{
	S_FBZCOLORPATH, S_FBZMODE, S_ALPHAMODE, S_FOGMODE,
	S_TEXMODE0, S_TEXMODE1,
	S_COLOR0, S_COLOR1, S_CHROMAKEY, S_FOGCOLOR,
	S_ZACOLOR, S_STIPPLE, S_ALPHAREF,
	S_CLIP_LR,                  // left | right << 16
	S_CLIP_TB,                  // top | bottom << 16
	S_BUFFER,                   // draw buffer selected by fbzMode
	S_TEX0, S_TEX1,             // texture slot of each TMU, ~0 when unused
	S_FLAGS,
	STATE_WORDS
};


// ======================> gpu_command

struct gpu_command
{
	enum class type : u8 { TRIANGLE, FASTFILL };

	type kind;
	u32 first_vertex;           // TRIANGLE: first of three vertices
	u32 prim;                   // index of the state record
};


// ======================> gpu_batch

class gpu_batch
{
public:
	// batches are closed at the latest when this many triangles have queued
	static constexpr u32 MAX_TRIANGLES = 1 << 20;

	// capture
	bool add_triangle(poly_data const &poly, bool y_origin, s32 yorigin, float const *x, float const *y);
	bool add_fastfill(poly_data const &poly);

	// lifecycle
	void reset();

	// getters
	std::vector<gpu_vertex> const &vertices() const { return m_vertices; }
	std::vector<u32> const &state() const { return m_state; }
	std::vector<gpu_command> const &commands() const { return m_commands; }
	u32 triangle_count() const { return m_triangles; }
	u32 fastfill_count() const { return m_fastfills; }
	u32 dropped_count() const { return m_dropped; }

private:
	u32 add_state(poly_data const &poly, u32 flags);

	std::vector<gpu_vertex> m_vertices;
	std::vector<u32> m_state;
	std::vector<gpu_command> m_commands;
	u32 m_triangles = 0;
	u32 m_fastfills = 0;
	u32 m_dropped = 0;
};

} // namespace voodoo

#endif // MAME_VIDEO_VOODOO_GPU_H
