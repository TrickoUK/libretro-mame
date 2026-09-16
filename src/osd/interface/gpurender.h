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

	// upload a full source texture (e.g. a device's entire VRAM) as a
	// single 2D texture, addressed in texels by gpu_vertex::u/v. Format is
	// always packed RGBA8 - the caller converts from whatever native pixel
	// format it stores internally.
	virtual void upload_texture(const uint32_t *rgba_pixels, int width, int height) = 0;

	// submit one triangle, drawn with the given blend mode against
	// whatever is already in the target. textured=false ignores u/v and
	// uses per-vertex color only.
	virtual void submit_triangle(const gpu_vertex tri[3], bool textured, gpu_blend_mode blend) = 0;

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

	// Temporarily give up whatever context this target holds current on
	// the calling thread, then reacquire it - for a caller that must let
	// another EGL/GL client (e.g. the frontend) safely do its own context
	// work in between, without the overhead of yielding around every
	// single call. Default no-op for any implementation that doesn't hold
	// a context this way (e.g. one that already re-acquires per-call).
	virtual void yield_context() {}
	virtual void resume_context() {}
};

} // namespace osd

#endif // MAME_OSD_INTERFACE_GPURENDER_H
