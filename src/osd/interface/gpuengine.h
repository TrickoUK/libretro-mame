// license:BSD-3-Clause
// copyright-holders:TrickoUK
/***************************************************************************

    gpuengine.h

    OSD interface to a frame-based GPU engine. A device builds one
    gpu_frame per emulated frame (textures to update, vertices, a table
    of per-primitive state and an ordered command list), submits it in a
    single synchronous call and gets the finished image back as ordinary
    32-bit pixels. The device supplies its pixel pipeline as a
    gpu_shader_module (GLSL fragment source); the engine owns contexts,
    textures, programs, depth and read-back.

    The interface holds no OpenGL types, so another backend can implement
    it. An OSD without an engine returns nullptr from
    osd_interface::get_gpu_engine(), and a device must then keep using its
    software renderer.

    Frame lifecycle rules (every adapter follows these):
    - Frame skip: the device does not submit a frame; the target is
      untouched and nothing is queued.
    - State load and reset: the device increments target.generation. A
      persistent target is cleared when the generation changes.
    - Resolution change: a different target size reallocates (and clears)
      the target. The device declares the largest size it will ever use to
      the frontend before the machine starts.
    - Failure: submit_frame() returns false when the GPU path cannot
      produce the frame (no context, a shader that does not compile, a
      bad frame description). The device then renders the frame in
      software and stops submitting once failed() is true.
    - Submission is synchronous. The engine makes its GPU context current
      only inside submit_frame() and releases it before returning, so it
      never stays current across separately scheduled emulator calls.

    Vertex layout (gpu_frame::vertices), per vertex, tightly packed:
        float x, y   target pixels, (0,0) = top-left of the target
        float z      depth in [0,1], used when the target depth mode is REAL
        float w      perspective divisor (1 = affine), larger = further away
        uint32_t prim  index into the per-primitive state table
        float v[varyings_per_vertex]
    The varyings are interpolated according to each module's
    varying_mode (perspective-correct, screen-linear or flat).

***************************************************************************/
#ifndef MAME_OSD_INTERFACE_GPUENGINE_H
#define MAME_OSD_INTERFACE_GPUENGINE_H

#pragma once

#include "gpurender.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>


namespace osd {

constexpr int GPU_MAX_VARYINGS = 16;
constexpr int GPU_MAX_BINDINGS = 8;

// opaque handle to an engine-owned object (texture or shader module);
// zero is never valid
using gpu_handle = uint32_t;
constexpr gpu_handle GPU_NULL_HANDLE = 0;

enum class gpu_texture_format : uint8_t
{
	R8UI,       // unsigned integer, sampled with texelFetch
	R16UI,
	R32UI,
	RG32UI,
	RGBA32I,    // signed integer, four words per texel
	RGBA8       // normalised colour, may be filtered
};

struct gpu_texture_desc
{
	gpu_texture_format format = gpu_texture_format::R16UI;
	int width = 0;
	int height = 0;
	bool filter_linear = false;     // RGBA8 only; integer formats are never filtered
};

// a rectangle of texels to replace; data points at the first texel of the
// rectangle, pitch_bytes is the distance between its rows (0 = tightly packed)
struct gpu_texture_update
{
	gpu_handle texture = GPU_NULL_HANDLE;
	int x = 0, y = 0, w = 0, h = 0;
	const void *data = nullptr;
	int pitch_bytes = 0;
};

enum class gpu_depth_mode : uint8_t
{
	NONE,       // draw order only, no depth attachment
	ORDER,      // the primitive index is the depth: with LESS the first primitive drawn at a pixel wins
	REAL        // vertex z is the depth, compared with each draw's depth_func
};

enum class gpu_depth_func : uint8_t
{
	NEVER, LESS, LEQUAL, EQUAL, GEQUAL, GREATER, NOTEQUAL, ALWAYS
};

enum class gpu_varying_mode : uint8_t
{
	PERSPECTIVE,
	NOPERSPECTIVE,
	FLAT
};

enum class gpu_blend_op : uint8_t { ADD, SUBTRACT, REVERSE_SUBTRACT };

enum class gpu_blend_factor : uint8_t
{
	ZERO, ONE, SRC_ALPHA, ONE_MINUS_SRC_ALPHA, CONSTANT_ALPHA, ONE_MINUS_CONSTANT_ALPHA
};

struct gpu_blend_state
{
	bool enable = false;
	gpu_blend_op op = gpu_blend_op::ADD;
	gpu_blend_factor src = gpu_blend_factor::ONE;
	gpu_blend_factor dst = gpu_blend_factor::ZERO;
	float constant_alpha = 1.0f;
};

// The pixel pipeline of one system. fragment_source is GLSL that defines
//     bool shade(out vec4 color)
// (return false to discard the fragment). The engine prepends a prelude:
//   - `float v0 ... v<varying_count-1>`: the interpolated varyings
//   - `int state(int k)`: word k of the current primitive's state record
//   - `vec2 frag_pos()`: the fragment position in target pixels, top-left origin
//   - `ivec2 target_size()`: the target size in pixels
//   - `VARIANT`: an integer macro, the draw's variant value
//   - for each binding i, a sampler named binding_name[i] (usampler2D for the
//     unsigned formats, isampler2D for RGBA32I, sampler2D for RGBA8) plus a
//     `ivec2 <name>_size()` helper
// The source stays within GLSL 3.30 so another backend can translate it.
struct gpu_shader_module
{
	const char *name = "";
	const char *fragment_source = "";
	int varying_count = 0;
	gpu_varying_mode varying_mode[GPU_MAX_VARYINGS] = {};
	int state_stride = 0;               // words per record the source reads
	int binding_count = 0;
	const char *binding_name[GPU_MAX_BINDINGS] = {};
	gpu_texture_format binding_format[GPU_MAX_BINDINGS] = {};
};

struct gpu_target_desc
{
	int width = 0;
	int height = 0;
	bool persistent = false;            // keep colour between frames; otherwise cleared to clear_color each frame
	gpu_depth_mode depth = gpu_depth_mode::NONE;
	int msaa_samples = 0;
	uint32_t generation = 0;            // a changed value clears a persistent target
	float clear_color[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
};

enum class gpu_command_type : uint8_t
{
	DRAW,           // draw vertices [first_vertex, first_vertex + vertex_count) as triangles
	SET_CLIP,       // scissor rectangle (x, y, w, h), top-left origin; persists until changed
	COPY_RECT,      // copy (x, y, w, h) to (dx, dy) inside the target, ignoring the clip
	UPDATE_TEXTURE, // replace a texture rectangle at this point of the frame
	CLEAR           // clear the whole target, ignoring the clip
};

struct gpu_command
{
	gpu_command_type type = gpu_command_type::DRAW;

	// DRAW
	gpu_handle module = GPU_NULL_HANDLE;
	uint32_t variant = 0;
	uint32_t first_vertex = 0;
	uint32_t vertex_count = 0;
	gpu_blend_state blend;
	gpu_depth_func depth_func = gpu_depth_func::ALWAYS;
	bool depth_write = false;
	gpu_handle bindings[GPU_MAX_BINDINGS] = {};

	// SET_CLIP, COPY_RECT
	int x = 0, y = 0, w = 0, h = 0;
	int dx = 0, dy = 0;

	// UPDATE_TEXTURE
	gpu_texture_update update;

	// CLEAR
	float clear_color[4] = {};
	bool clear_color_enable = true;
	bool clear_depth_enable = true;

	static gpu_command make_draw(gpu_handle module, uint32_t first_vertex, uint32_t vertex_count)
	{
		gpu_command c;
		c.type = gpu_command_type::DRAW;
		c.module = module;
		c.first_vertex = first_vertex;
		c.vertex_count = vertex_count;
		return c;
	}

	static gpu_command make_clip(int x, int y, int w, int h)
	{
		gpu_command c;
		c.type = gpu_command_type::SET_CLIP;
		c.x = x; c.y = y; c.w = w; c.h = h;
		return c;
	}

	static gpu_command make_copy(int sx, int sy, int dx, int dy, int w, int h)
	{
		gpu_command c;
		c.type = gpu_command_type::COPY_RECT;
		c.x = sx; c.y = sy; c.dx = dx; c.dy = dy; c.w = w; c.h = h;
		return c;
	}

	static gpu_command make_update(const gpu_texture_update &u)
	{
		gpu_command c;
		c.type = gpu_command_type::UPDATE_TEXTURE;
		c.update = u;
		return c;
	}

	static gpu_command make_clear(float r, float g, float b, float a)
	{
		gpu_command c;
		c.type = gpu_command_type::CLEAR;
		c.clear_color[0] = r; c.clear_color[1] = g; c.clear_color[2] = b; c.clear_color[3] = a;
		return c;
	}
};

// One frame of work. Every pointer must stay valid until submit_frame() returns.
struct gpu_frame
{
	gpu_target_desc target;

	// texture rectangles applied before the first command
	const gpu_texture_update *updates = nullptr;
	int update_count = 0;

	const void *vertices = nullptr;     // layout in the file header
	uint32_t vertex_count = 0;
	int varyings_per_vertex = 0;        // at least every module's varying_count

	// per-primitive state: prim_count records of state_stride words each
	const uint32_t *prim_state = nullptr;
	uint32_t prim_count = 0;            // primitive ids in the vertices are below this
	int state_stride = 0;

	const gpu_command *commands = nullptr;
	int command_count = 0;

	// regions read back at the end of the frame (target pixels, row 0 = top)
	const gpu_readback_rect *readbacks = nullptr;
	int readback_count = 0;
};

struct gpu_engine_caps
{
	int gl_major = 0, gl_minor = 0;
	int max_texture_size = 0;
	int max_samples = 0;
	const char *renderer = "";
};

// timing of the last submitted frame, in milliseconds
struct gpu_engine_stats
{
	double upload_ms = 0.0;
	double draw_ms = 0.0;
	double readback_ms = 0.0;
	uint64_t uploaded_bytes = 0;
	uint32_t draw_calls = 0;
	uint32_t program_compiles = 0;      // programs built during this frame
};

class gpu_engine
{
public:
	virtual ~gpu_engine() = default;

	virtual const gpu_engine_caps &caps() const = 0;

	// false after an unrecoverable failure; the device keeps its software path
	virtual bool failed() const = 0;

	// create a texture (contents undefined until updated); returns GPU_NULL_HANDLE
	// when the size, format or the memory budget cannot be satisfied
	virtual gpu_handle create_texture(const gpu_texture_desc &desc) = 0;
	virtual void destroy_texture(gpu_handle texture) = 0;
	virtual uint64_t texture_memory() const = 0;
	virtual void set_texture_budget(uint64_t bytes) = 0;

	// register a pixel pipeline; programs are built on first use. Returns
	// GPU_NULL_HANDLE when the description is invalid.
	virtual gpu_handle create_shader_module(const gpu_shader_module &module) = 0;
	virtual void destroy_shader_module(gpu_handle module) = 0;

	// run one frame; see the file header for the lifecycle rules
	virtual bool submit_frame(const gpu_frame &frame) = 0;

	virtual const gpu_engine_stats &last_frame_stats() const = 0;
};


// Tracks which part of a CPU-side texture shadow changed since the last
// upload, as one bounding rectangle. A device marks writes from its memory
// handlers and turns the result into a gpu_texture_update once per frame.
// Setting MAME_GPU_FORCE_FULL_UPLOAD in the environment makes every take()
// return the whole texture, to rule out dirty tracking when textures look
// stale.
class gpu_dirty_region
{
public:
	gpu_dirty_region(int width, int height) : m_width(width), m_height(height) { mark_all(); }

	void mark(int x, int y, int w, int h)
	{
		if (w <= 0 || h <= 0)
			return;
		const int x1 = std::max(x, 0);
		const int y1 = std::max(y, 0);
		const int x2 = std::min(x + w, m_width);
		const int y2 = std::min(y + h, m_height);
		if (x1 >= x2 || y1 >= y2)
			return;
		if (!m_dirty)
		{
			m_x1 = x1; m_y1 = y1; m_x2 = x2; m_y2 = y2;
			m_dirty = true;
		}
		else
		{
			m_x1 = std::min(m_x1, x1); m_y1 = std::min(m_y1, y1);
			m_x2 = std::max(m_x2, x2); m_y2 = std::max(m_y2, y2);
		}
	}

	void mark_all() { m_dirty = false; mark(0, 0, m_width, m_height); }

	bool dirty() const { return m_dirty; }

	// returns false when nothing changed; otherwise the rectangle to upload,
	// and the region becomes clean
	bool take(int &x, int &y, int &w, int &h)
	{
		static const bool force_full = std::getenv("MAME_GPU_FORCE_FULL_UPLOAD") != nullptr;
		if (force_full)
			m_dirty = true, m_x1 = 0, m_y1 = 0, m_x2 = m_width, m_y2 = m_height;
		if (!m_dirty)
			return false;
		x = m_x1; y = m_y1; w = m_x2 - m_x1; h = m_y2 - m_y1;
		m_dirty = false;
		return true;
	}

private:
	int m_width, m_height;
	bool m_dirty = false;
	int m_x1 = 0, m_y1 = 0, m_x2 = 0, m_y2 = 0;
};

} // namespace osd

#endif // MAME_OSD_INTERFACE_GPUENGINE_H
