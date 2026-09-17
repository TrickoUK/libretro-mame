// license:BSD-3-Clause
// copyright-holders:TrickoUK
/***************************************************************************

    gpurender.h

    OSD interface to an independent, headless GPU render target that
    device code can use to rasterize real geometry (instead of a software
    scanline rasterizer) at an arbitrary internal resolution, reading the
    result back into a normal bitmap_rgb32.

    Deliberately minimal and vertex-format-agnostic (position/color/UV
    triangles only) - a device is expected to translate its own primitive
    types (quads, fans, etc.) into triangles before submitting them.

    An OSD that doesn't implement GPU rendering returns nullptr from
    osd_interface::get_gpu_render_target() (the default, provided by
    osd_common_t), so calling device code must always be prepared to fall
    back to its existing software path - this is not a required feature of
    any OSD.

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
	float r, g, b, a;
	float u, v;
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

// an independent, headless GPU render target. One instance renders one
// screen's worth of geometry per frame; not thread-safe, not shared
// between devices.
class gpu_render_target
{
public:
	virtual ~gpu_render_target() = default;

	// (re)size the off-screen target if needed and clear it. width/height
	// are in target pixels (i.e. native resolution * desired multiplier).
	virtual void begin_frame(int width, int height) = 0;

	// upload a device's entire raw VRAM as a single integer texture (one
	// 16-bit texel per VRAM word, unpacked/undecoded) - addressed in whole
	// VRAM words by set_texture_page() below, not by gpu_vertex::u/v
	// directly. Meant to be called once per frame (VRAM is uploaded
	// wholesale, cheaply, rather than decoding/re-uploading a texture page
	// worth of pixels every time a polygon's texture page/CLUT changes).
	virtual void upload_vram(const uint16_t *vram_words, int width, int height) = 0;

	// select which texture page/CLUT/pixel-format subsequent textured
	// submit_triangle(s) calls should sample from within the last
	// upload_vram()'d data, in VRAM word coordinates (matching
	// upload_vram's addressing) - tp: 0 = 4bpp CLUT, 1 = 8bpp CLUT,
	// 2 = 16bpp direct, mirroring the PS1 GPU's own texture-page
	// color-mode field. gpu_vertex::u/v remain page-relative (0-255).
	virtual void set_texture_page(int tx, int ty, int tp, int clutx, int cluty) = 0;

	// submit one triangle, drawn with the given blend mode against
	// whatever is already in the target. textured=false ignores u/v and
	// uses per-vertex color only.
	virtual void submit_triangle(const gpu_vertex tri[3], bool textured, gpu_blend_mode blend) = 0;

	// submit `count` vertices (a multiple of 3, i.e. count/3 triangles) as
	// one batch, all sharing the same textured/blend state - equivalent to
	// calling submit_triangle() count/3 times, but as a single underlying
	// draw call. Significantly cheaper for many triangles sharing state
	// (the common case - see psxgpu_device::gpu_update_screen(), which
	// groups its queued triangles into runs before calling this). Default
	// implementation just loops submit_triangle() for callers/backends
	// that don't need the batched path.
	virtual void submit_triangles(const gpu_vertex *verts, int count, bool textured, gpu_blend_mode blend)
	{
		for (int i = 0; i + 3 <= count; i += 3)
			submit_triangle(&verts[i], textured, blend);
	}

	// finish rendering and read the target back into a caller-owned RGBA8
	// buffer (width*height*4 bytes, row 0 = top, matching bitmap_rgb32
	// convention.
	virtual void end_frame_and_readback(uint32_t *rgba_out) = 0;

	// restrict subsequent submit_triangle() calls to an inclusive
	// rectangle in the same target-pixel space as gpu_vertex positions
	// (0,0 = top-left), matching hardware with a settable clip/draw-area
	// register (e.g. the PS1 GPU's draw area). Default no-op for any
	// implementation/device pairing that doesn't need it.
	virtual void set_clip_rect(int x1, int y1, int x2, int y2) {}

	// copy a w x h rectangle within the target itself, from (sx,sy) to
	// (dx,dy) - both corners in the same target-pixel space as gpu_vertex
	// positions. Ignores the current clip rect (matching hardware VRAM-to-
	// VRAM copy commands, which bypass the draw-area clip entirely).
	// Default no-op for any implementation/device pairing that doesn't
	// need it.
	virtual void copy_rect(int sx, int sy, int dx, int dy, int w, int h) {}

	// Optionally batch a run of upload_vram()/set_texture_page()/set_clip_rect()/
	// submit_triangle() calls under a single context acquisition instead
	// of each call acquiring its own - a significant win when many calls
	// happen back-to-back with nothing else (e.g. no return to the host's
	// own scheduler/frontend) interleaved between them. Only call these
	// around a tight, synchronous run of calls where the caller can
	// guarantee nothing else needs this thread's context in between -
	// see retro_gpu_target's implementation for the concrete rationale.
	// Default no-op pair for any implementation that doesn't support or
	// need this (every call then just acquires its own context, as if
	// begin_batch()/end_batch() were never called).
	virtual void begin_batch() {}
	virtual void end_batch() {}
};

} // namespace osd

#endif // MAME_OSD_INTERFACE_GPURENDER_H
