// license:BSD-3-Clause
// copyright-holders:TrickoUK
/***************************************************************************

    gpurender.h

    Primitive types shared by devices that feed the host-GPU engine (see
    gpuengine.h): the PS1 GPU's vertex, its blend modes and a read-back
    rectangle. Positions are in target pixel space, (0,0) = top-left of the
    render target.

***************************************************************************/
#ifndef MAME_OSD_INTERFACE_GPURENDER_H
#define MAME_OSD_INTERFACE_GPURENDER_H

#pragma once

#include "osdcomm.h"

#include <cstdint>


namespace osd {

// a single (position, color, UV) vertex; position is in target pixel
// space (0,0 = top-left of the render target), not normalized device
// coordinates - the implementation handles that conversion.
struct gpu_vertex
{
	float x, y;
	// Perspective divisor for varying (color/UV) interpolation only - the
	// final screen position (x,y) is unaffected by this value regardless
	// of what it's set to (see retro_gpu_target's vertex shader). 1.0
	// (the default) means affine interpolation, matching original PS1
	// hardware and every vertex without real PGXP-style depth data; set
	// to a vertex's real relative depth (larger = further from the
	// camera) to enable perspective-correct interpolation for that
	// vertex instead.
	float w = 1.0f;
	float r, g, b, a;
	float u, v;
	// Texel-space rectangle (same units as u/v) that a filtered (see
	// submit_triangle(s)'s filterable parameter) sample is allowed to pull
	// neighbor taps from - the source primitive's own UV footprint, i.e.
	// the min/max of u/v across all of that primitive's vertices. Bilinear/
	// trilinear taps landing outside this rectangle are clamped back into
	// it (clamp-to-edge) instead of reading whatever unrelated texture
	// data happens to be packed next to it in the same shared VRAM texture
	// page - without this, filtering a real primitive can visibly bleed in
	// a neighboring, unrelated sprite/texture's colors at its edges.
	// Defaults to a whole 256x256 texture page (the maximum any texture
	// page addressing mode can reach), a safe fallback for any submitter
	// that doesn't compute a tighter bound.
	float u_min = 0.0f, v_min = 0.0f, u_max = 255.0f, v_max = 255.0f;
};

// PS1-style semi-transparency blend modes (also usable by other future
// devices with similar fixed blend-equation hardware); NONE disables
// blending (opaque draw).
enum class gpu_blend_mode
{
	NONE,
	HALF_ADD,       // 0.5*back + 0.5*front
	ADD,            // 1.0*back + 1.0*front
	SUBTRACT,       // 1.0*back - 1.0*front
	ADD_QUARTER     // 1.0*back + 0.25*front
};

// a rectangle of the render target to read back, in top-left-origin
// target pixels, and the caller-owned buffer receiving it (row 0 = top,
// matching bitmap_rgb32 convention). stride is the distance between
// output rows in pixels.
//
// By default the pixels come back as they are in the target. The optional
// fields run a final pass on the GPU before the copy: the output is then
// (w / downsample) x (h / downsample) pixels.
enum class gpu_readback_format : uint8_t
{
	BGRA8,      // out: 32-bit pixels
	RG8         // out16: the red byte is the low byte and the green byte the high byte of a 16-bit value
};

struct gpu_readback_rect
{
	int x = 0, y = 0, w = 0, h = 0;
	uint32_t *out = nullptr;
	int stride = 0;

	gpu_readback_format format = gpu_readback_format::BGRA8;
	uint16_t *out16 = nullptr;              // RG8 only, in place of out; stride counts 16-bit values; no output pass

	int downsample = 1;                     // average this many target pixels (each way) into one output pixel; w and h must be multiples
	bool opaque = false;                    // force alpha to 1
	const uint8_t *channel_lut = nullptr;   // 128 bytes: output levels for the 5-bit red (32), 6-bit green (64) and 5-bit blue (32) value of the averaged colour

	bool plain() const { return downsample == 1 && !opaque && !channel_lut; }
};

} // namespace osd

#endif // MAME_OSD_INTERFACE_GPURENDER_H
