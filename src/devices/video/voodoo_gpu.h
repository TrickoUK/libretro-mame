// license:BSD-3-Clause
// copyright-holders:TrickoUK
/***************************************************************************

    voodoo_gpu.h

    Host-GPU path of the 3dfx Voodoo renderer. A gpu_batch captures the
    triangles and fast fills that reach the rasterizer queue; a gpu_renderer
    turns a batch into one engine frame (see osd/interface/gpuengine.h).

    Colour buffers are stacked vertically in one persistent engine target
    (buffer k occupies rows k * H to (k + 1) * H - 1, times the render
    scale), with a REAL depth attachment of the same size.

    State record (STATE_WORDS words per primitive, raw register values):
    see the S_* indices below. Each TMU block holds the decoded texture
    state: the nine LOD offsets, width and height masks, LOD limits and
    bias, the LOD mask, detail parameters, the texture RAM mask, the
    palette row and the base LOD.

***************************************************************************/

#ifndef MAME_VIDEO_VOODOO_GPU_H
#define MAME_VIDEO_VOODOO_GPU_H

#pragma once

#include "voodoo_regs.h"
#include "voodoo_render.h"

#include "interface/gpuengine.h"

#include <map>
#include <memory>
#include <string>
#include <vector>


namespace voodoo
{

// ======================> gpu_vertex

// One triangle corner, in the layout the engine expects (x, y, z, w, prim,
// varyings). The iterated values are the plane equations of the triangle
// evaluated at the corner, shifted half a pixel so that the sample at a
// pixel centre equals the value the scanline rasterizer computes at the
// integer pixel.
struct gpu_vertex
{
	static constexpr int VARYINGS = 12;

	float x, y;                 // target pixels
	float z, w;
	u32 prim;                   // index of the state record
	float v[VARYINGS];          // see V_*
};

enum : int
{
	V_R, V_G, V_B, V_A,         // iterated colour, 0-255 scale
	V_Z,                        // iterated Z, 20.12
	V_W,                        // iterated W, 16.32
	V_S0, V_T0, V_W0,           // TMU 0 S, T (14.18) and W (2.30)
	V_S1, V_T1, V_W1            // TMU 1
};


// ======================> state record layout

enum : int
{
	S_FBZCOLORPATH, S_FBZMODE, S_ALPHAMODE, S_FOGMODE,
	S_TEXMODE0, S_TEXMODE1,
	S_COLOR0, S_COLOR1, S_CHROMAKEY, S_FOGCOLOR,
	S_ZACOLOR, S_STIPPLE, S_ALPHAREF,
	S_CLIP_LR,                  // left | right << 16 (fast fill: the columns filled)
	S_CLIP_TB,                  // top | bottom << 16 (fast fill: the rows filled)
	S_BUFFER,                   // buffer index (0-2)
	S_FLAGS,                    // bit 0 fast fill, bit 1 LFB stamp, bit 2 TMU 0 combine is identity, bit 3 TMU 1 combine is identity
	S_CHROMARANGE,
	S_FOGROW,                   // row of the fog table
	S_FOGMASK,                  // fog delta mask
	S_BILINMASK,                // texture bilinear fraction mask
	S_BUFROW,                   // first target row of the buffer, native pixels (set at submit)
	S_SCALE,                    // render scale (set at submit)
	S_TMU0 = 24,
	S_TMU1 = 48,
	STATE_WORDS = 72
};

// word offsets inside a TMU block
enum : int
{
	TB_LODOFF = 0,              // nine words
	TB_WMASK = 9, TB_HMASK, TB_LODMIN, TB_LODMAX, TB_LODBIAS, TB_LODMASK,
	TB_DETMAX, TB_DETBIAS, TB_DETSCALE, TB_MASK, TB_LUTROW, TB_LODBASE
};


// ======================> gpu_command

struct gpu_command
{
	enum class type : u8 { TRIANGLE, FASTFILL, STAMP };

	type kind;
	u32 first_vertex;           // TRIANGLE: first of three vertices; STAMP: index of the stamp
	u32 prim;                   // index of the state record
};


// ======================> gpu_batch

class gpu_batch
{
public:
	static constexpr u32 MAX_TRIANGLES = 1 << 20;
	static constexpr u32 LUT_ROWS = 1024;
	static constexpr u32 FOG_ROWS = 256;

	// register state that is not part of poly_data
	struct globals
	{
		u32 chroma_range;
		u8 const *fogblend;
		u8 const *fogdelta;
		u8 fogdelta_mask;
		u8 bilinear_mask;
		rgb_t const *argb1555;      // texel tables, to tell the 16-bit formats apart
		rgb_t const *argb4444;
	};

	// the reason a polygon cannot be drawn on the GPU, or nullptr
	static char const *check(poly_data const &poly);

	// capture; buffer is the colour buffer index, rows are native pixels
	void add_triangle(poly_data const &poly, int buffer, bool y_origin, s32 yorigin, float const *x, float const *y, globals const &g);
	void add_fastfill(poly_data const &poly, int buffer, bool y_origin, s32 yorigin);

	// raw linear frame buffer writes: coloured pixels (opaque argb) of a buffer, grouped into stamps
	struct stamp
	{
		int buffer, x, y, w, h;
		std::vector<u32> pixels;    // w * h, zero where nothing was written
		std::vector<u32> depth;     // w * h, bit 16 set plus the 16-bit depth where written; empty if none
	};
	void set_geometry(u32 rowpixels, u32 rows);
	void add_lfb_pixel(int buffer, int x, int y, u32 argb);
	void add_lfb_depth(int buffer, int x, int y, u16 depth);
	std::vector<stamp> const &stamps() const { return m_stamps; }

	// true when a table pool is nearly full and the batch should be submitted first
	bool needs_flush() const { return m_triangles >= MAX_TRIANGLES || m_lut_rows + 2 >= LUT_ROWS || m_fog_rows + 1 >= FOG_ROWS; }

	// texture RAM bytes the batch reads so far (empty when lo >= hi)
	bool texture_hazard(int tmu, u32 offs, u32 bytes) const;

	void reset();

	// getters
	std::vector<gpu_vertex> const &vertices() const { return m_vertices; }
	std::vector<u32> const &state() const { return m_state; }
	std::vector<gpu_command> const &commands() const { return m_commands; }
	u32 const *lut() const { return m_lut.data(); }
	u32 lut_rows() const { return m_lut_rows; }
	u32 const *fog() const { return m_fog.data(); }
	u32 fog_rows() const { return m_fog_rows; }
	u32 triangle_count() const { return m_triangles; }
	u32 fastfill_count() const { return m_fastfills; }
	char const *unsupported() const { return m_unsupported; }

	void close_stamp();

private:
	u32 add_state(poly_data const &poly, u32 flags, int buffer);
	void fill_tmu(u32 *block, int tmu, rasterizer_texture const *tex, s64 dsdx, s64 dsdy, s64 dtdx, s64 dtdy);
	u32 lut_row(int tmu, rgb_t const *table);
	u32 fog_row(u8 const *blend, u8 const *delta);

	std::vector<gpu_vertex> m_vertices;
	std::vector<u32> m_state;
	std::vector<gpu_command> m_commands;
	std::vector<stamp> m_stamps;
	std::vector<u32> m_stage;               // pixels of the open stamp, rowpixels * rows
	std::vector<u32> m_stage_z;             // depth of the open stamp, allocated on the first depth write
	bool m_stage_has_z = false;
	u32 m_stage_rowpixels = 0, m_stage_rows = 0;
	int m_stage_buffer = -1;
	int m_stage_x0 = 0, m_stage_y0 = 0, m_stage_x1 = -1, m_stage_y1 = -1;
	std::vector<u32> m_lut = std::vector<u32>(LUT_ROWS * 256);
	std::vector<u32> m_fog = std::vector<u32>(FOG_ROWS * 64);
	u32 m_lut_rows = 0;
	u32 m_fog_rows = 0;
	u32 m_lut_last[2] = { ~0U, ~0U };
	u32 m_tex_lo[2] = { ~0U, ~0U };
	u32 m_tex_hi[2] = { 0, 0 };
	u32 m_tex_mask[2] = { 0, 0 };
	u32 m_triangles = 0;
	u32 m_fastfills = 0;
	char const *m_unsupported = nullptr;
};


// ======================> gpu_renderer

class gpu_renderer
{
public:
	gpu_renderer(running_machine &machine, osd::gpu_engine *engine);
	~gpu_renderer();

	// create the engine objects; false leaves the renderer unusable
	bool init(u8 *tmu0_ram, u32 tmu0_size, u8 *tmu1_ram, u32 tmu1_size);

	bool usable() const { return m_engine != nullptr && !m_engine->failed() && m_ok && !m_latched; }
	gpu_batch &batch() { return m_batch; }
	int scale() const { return m_scale; }
	u32 serial() const { return m_serial; }
	u32 rowpixels() const { return m_rowpixels; }
	u32 rows() const { return m_rows; }

	// an unsupported access was seen: the device continues in software
	void latch(char const *reason);

	// size of the colour buffer stack; a change clears the target
	void configure(u32 rowpixels, u32 rows, int buffers);

	// mark a texture RAM range changed (after submitting if the batch reads it)
	void texture_write(int tmu, u32 offs, u32 bytes);
	void invalidate();

	// submit the batch; with a buffer and output it also reads rows of that
	// buffer back as rgb32 (row_count 0 = all rows). read_first reads before the
	// batch is drawn, which is only right when the batch does not write that buffer;
	// the draws then run on the GPU while emulation continues.
	// out_stride (pixels, 0 = tightly packed, in output pixels), opaque, channel_lut and downsample (see gpu_readback_rect) run a final pass on the GPU
	bool flush(int read_buffer = -1, u32 *out = nullptr, u32 first_row = 0, u32 row_count = 0, bool read_first = false,
			u32 out_stride = 0, bool opaque = false, u8 const *channel_lut = nullptr, int downsample = 1);

	// true if any command of the batch can change the colour of the buffer slot
	bool batch_writes_buffer(int buffer) const;

	// per-stage timers, printed by profile_report() when MAME_VOODOO_GPU_PROF is set in the environment
	static bool profiling();
	void why(char const *reason) { m_why = reason; }
	void profile_capture(double ms) { m_prof.capture_ms += ms; m_prof.capture_count++; }
	void profile_present(double ms) { m_prof.present_ms += ms; }
	void profile_count(char const *name, u32 n = 1) { m_prof.reasons[name] += n; }
	void profile_report();

private:
	struct profile_data
	{
		double capture_ms = 0, build_ms = 0, upload_ms = 0, draw_ms = 0, gpu_ms = 0, readback_ms = 0, submit_ms = 0, present_ms = 0;
		u64 capture_count = 0, flushes = 0, triangles = 0, fills = 0, vertices = 0, states = 0, uploaded = 0, draws = 0, readbacks = 0;
		std::map<std::string, u32> reasons;
		u32 presents = 0;
	};
	char const *m_why = nullptr;
	profile_data m_prof;
	running_machine &m_machine;
	osd::gpu_engine *m_engine;
	bool m_ok = false;
	bool m_shared_ram = false;              // both TMUs address the same memory
	bool m_latched = false;                 // an unsupported mode was seen: software only from now on
	osd::gpu_handle m_module = osd::GPU_NULL_HANDLE;
	osd::gpu_handle m_tmu_tex[2] = {};
	osd::gpu_handle m_lut_tex = osd::GPU_NULL_HANDLE;
	osd::gpu_handle m_fog_tex = osd::GPU_NULL_HANDLE;
	osd::gpu_handle m_stamp_tex = osd::GPU_NULL_HANDLE;
	osd::gpu_handle m_stampz_tex = osd::GPU_NULL_HANDLE;
	u8 *m_ram[2] = {};
	u32 m_ram_size[2] = {};
	std::unique_ptr<osd::gpu_dirty_region> m_dirty[2];
	gpu_batch m_batch;
	u32 m_rowpixels = 0, m_rows = 0;
	int m_buffers = 0;
	int m_scale = 1;
	u32 m_generation = 1;
	u32 m_serial = 0;
};

} // namespace voodoo

#endif // MAME_VIDEO_VOODOO_GPU_H
