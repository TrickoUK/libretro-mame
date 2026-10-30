// license:BSD-3-Clause
// copyright-holders:Philip Bennett
/***************************************************************************

    3DO M2 BDA Triangle Engine

***************************************************************************/
#ifndef MAME_KONAMI_3DOM2_TE_H
#define MAME_KONAMI_3DOM2_TE_H

#pragma once

#include <array>
#include <memory>
#include <mutex>
#include <vector>

/***************************************************************************
    FORWARD DECLARATIONS
***************************************************************************/

class m2_bda_device;

/***************************************************************************
    TRIANGLE ENGINE DEVICE
***************************************************************************/

class m2_te_device : public device_t
{
public:
	m2_te_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock);

	// Static configuration helpers
	auto general_int_handler() { return m_general_int_handler.bind(); }
	auto dfinstr_int_handler() { return m_dfinstr_int_handler.bind(); }
	auto iminstr_int_handler() { return m_iminstr_int_handler.bind(); }
	auto listend_int_handler() { return m_listend_int_handler.bind(); }
	auto winclip_int_handler() { return m_winclip_int_handler.bind(); }

	uint32_t read(offs_t offset);
	void write(offs_t offset, uint32_t data);

	uint32_t *tram_ptr() const { return &m_tram[0]; }

	enum te_reg_wmode
	{
		REG_WRITE,
		REG_INVALID,
		REG_SET,
		REG_CLEAR,
	};

protected:
	virtual void device_start() override ATTR_COLD;
	virtual void device_reset() override ATTR_COLD;
	virtual void device_post_load() override;

	TIMER_CALLBACK_MEMBER(command_done);

private:

	enum te_state
	{
		TE_STOPPED,
		TE_PAUSED,
		TE_RUNNING,
	};

	enum misc
	{
		PIP_RAM_WORDS           = 256,
		TEXTURE_RAM_WORDS       = 4096,
		PIP_RAM_BYTEMASK        = PIP_RAM_WORDS * 4 - 1,
		TEXTURE_RAM_BYTEMASK    = TEXTURE_RAM_WORDS * 4 - 1,
	};

	enum inst_type
	{
		INST_WRITE_REG  = 0x10000000,
		INST_VTX_SHORT  = 0x20000000,
		INST_VTX_LONG   = 0x30000000,
		INST_VTX_POINT  = 0x40000000,
		INST_MASK       = 0xf0000000,
	};

	enum vtx_flag
	{
		VTX_FLAG_SHAD   = 0x00010000,
		VTX_FLAG_TEXT   = 0x00020000,
		VTX_FLAG_PRSP   = 0x00040000,
		VTX_FLAG_NEW    = 0x00080000,
		VTX_FLAG_RM     = 0x00100000,
	};

	struct se_vtx
	{
		float x, y;
		float r, g, b, a;
		float w;
		float uw, vw;
		float iv[8];
	};

	struct slope_params
	{
		float y23;
		float y31;
		float y12;
		float x23;
		float x31;
		float x12;
		float xstep_long;
		float iAria;
	};

	struct rgba { uint8_t r, g, b, a; };

	// Defined below (after m_gc/m_es/m_tm/m_db, whose types it captures via
	// decltype) - see the definition for what this is and why it exists.
	struct render_snapshot;
	struct span_regs;

	// A supersampled copy of one framebuffer or Z buffer that the Triangle
	// Engine draws into when supersampling is on.  RAM stays the truth: the
	// samples are refreshed from RAM when a list first draws to the surface
	// (rows that differ from the last write-back) and averaged back down into
	// RAM when the list ends.
	struct ss_surface
	{
		uint32_t base = 0;          // RAM address of native row 0
		uint32_t stride = 0;        // native pixels per RAM row
		uint32_t bpp = 0;           // bytes per native pixel
		bool is_z = false;
		uint32_t width = 0;         // native pixels
		uint32_t height = 0;
		uint32_t pitch = 0;         // samples per row
		uint32_t rows = 0;          // sample rows
		bool valid = false;
		uint32_t gen = ~uint32_t(0);    // list generation this surface was last refreshed for
		uint32_t row_slot = 0;      // bytes of native_copy per native row
		std::unique_ptr<uint32_t []> data;
		std::unique_ptr<uint8_t []> native_copy;
		std::unique_ptr<uint8_t []> dirty;      // one flag per sample row
		bool touched = false;
	};

	// Per-render-batch scratch: destination-blender working state (previously
	// the member field m_dbstate) plus locally-accumulated status/statistics
	// bits. One instance is owned per render batch (currently: one per
	// worker-thread band, reused across every queued span_job that band
	// processes) rather than being a shared device member, so this chain is
	// safe to run from multiple worker threads at once - each band's
	// pixel_scratch is private to that band's thread. status_bits/stats are
	// OR'd/added into the real m_db.status/g_statistics only after every
	// band has finished (osd_work_queue_wait() returned, single-threaded
	// merge), never touched directly from more than one thread at a time.
	struct pixel_scratch
	{
		uint32_t    x;
		uint32_t    y;
		uint32_t    w;

		rgba    ti;
		uint8_t ssb;

		rgba    src;
		uint8_t dsb;

		rgba    srcpath;
		rgba    texpath;
		rgba    blend;
		rgba    dst;

		uint32_t status_bits = 0;
		uint32_t stats[16] = {};

		// Plane gradients per sample row of the job being drawn (supersampling)
		uint32_t ddy_uw = 0;
		uint32_t ddy_vw = 0;
		uint32_t ddy_w = 0;

		// Which config generation this job's render calls should read from -
		// set once per job by render_band() from span_job::snapshot before
		// walk_span() is called, not owned/kept alive by pixel_scratch itself.
		const render_snapshot *cfg = nullptr;
	};

	// One rasterized scanline's worth of walk_span() arguments, captured so
	// it can be handed to a worker thread instead of being rendered
	// synchronously inline. Bucketed by row (y % NUM_RENDER_BANDS) into
	// m_span_jobs - every row is always processed by the same band, so a
	// band's jobs always execute in original submission order and different
	// bands never write the same framebuffer row, making this safe to farm
	// out to real worker threads with no locking needed on the framebuffer
	// itself (see queue_span_job()/flush_span_jobs()).
	struct span_job
	{
		uint32_t wrange;
		bool omit_right;
		uint32_t y, xs, xe;
		int32_t r, g, b, a;
		uint32_t uw, vw, w;

		// m_es.r2l/ddx_* are recomputed per-triangle by setup_triangle(),
		// not just by explicit CPU register writes - since jobs from
		// multiple triangles can sit queued at once (only flushed at
		// INST_WRITE_REG/list-end boundaries, not between triangles), these
		// must be captured here at queue time rather than read live from
		// m_es when the job actually renders, or a later triangle's setup
		// would silently corrupt an earlier triangle's still-queued jobs.
		uint32_t r2l;
		uint32_t ddx_r, ddx_g, ddx_b, ddx_a;
		uint32_t ddx_uw, ddx_vw, ddx_w;
		uint32_t ddy_uw, ddy_vw, ddy_w;

		// Config generation active when this job was queued (see
		// render_snapshot) - lets jobs queued under different register-write
		// generations sit in the same m_span_jobs list and still each render
		// against the correct config, so INST_WRITE_REG no longer has to
		// flush+wait before applying a register write (see execute()).
		std::shared_ptr<const render_snapshot> snapshot;
	};

	static constexpr int NUM_RENDER_BANDS = 64;

	// Per-band render state shared between the emulation thread (producer)
	// and whichever worker thread is currently draining the band. The
	// emulation thread appends published jobs to pending under the lock and
	// schedules a render_band() item only if none is already draining the
	// band; the item keeps taking pending batches until it finds none, so a
	// band's jobs always run in submission order on one thread at a time.
	// ps accumulates the band's status/statistics until flush_span_jobs()
	// merges and resets it.
	struct band_state
	{
		m2_te_device *dev = nullptr;
		int band = 0;
		std::mutex lock;
		std::vector<span_job> pending;
		std::vector<span_job> work;
		bool scheduled = false;
		pixel_scratch ps;
	};

	void queue_span_job(uint32_t wrange, bool omit_right, uint32_t y, uint32_t xs, uint32_t xe, int32_t r, int32_t g, int32_t b, int32_t a, uint32_t uw, uint32_t vw, uint32_t w);
	void publish_span_jobs();

	// Supersampling (log2 of the scale per axis; 0 = off)
	uint32_t m_ss_shift = 0;
	uint32_t m_ss_ddy_uw = 0;
	uint32_t m_ss_ddy_vw = 0;
	uint32_t m_ss_ddy_w = 0;
	uint32_t m_list_gen = 0;
	std::vector<std::unique_ptr<ss_surface>> m_ss_surfaces;
	ss_surface *ss_get(uint32_t base, uint32_t stride, uint32_t bpp, bool is_z, uint32_t width, uint32_t height, bool create);
	void ss_acquire(ss_surface &s);
	void ss_resolve(render_snapshot &snap);
	void ss_writeback();
	void ss_writeback_rows(ss_surface &s, uint32_t row_begin, uint32_t row_end);
	static void *ss_writeback_work(void *param, int threadid);

	// One chunk of a surface's write-back, run on a worker thread
	struct ss_writeback_item
	{
		m2_te_device *dev;
		ss_surface *surf;
		uint32_t row_begin;
		uint32_t row_end;
	};
	static constexpr uint32_t SS_WRITEBACK_CHUNKS = 24;
	static constexpr uint32_t SS_WRITEBACK_MIN_ROWS = 16;  // fewer changed rows are written back inline
	std::array<ss_writeback_item, SS_WRITEBACK_CHUNKS> m_wb_items;
	uint32_t ss_read(const ss_surface &s, uint32_t x, uint32_t y) const { return (x < s.pitch && y < s.rows) ? s.data[y * s.pitch + x] : 0; }
	// The dirty flags of neighbouring rows share cache lines and the rows are
	// drawn by different worker threads, so a flag is only stored when it changes
	void ss_write(ss_surface &s, uint32_t x, uint32_t y, uint32_t v) { if (x < s.pitch && y < s.rows) { s.data[y * s.pitch + x] = v; if (!s.dirty[y]) s.dirty[y] = 1; } }
	void flush_span_jobs();
	static void *render_band(void *param, int threadid);

	void set_interrupt(uint32_t mask);
	void update_interrupts();

	void teicntl_w(uint32_t data, te_reg_wmode wmode);
	void tedcntl_w(uint32_t data, te_reg_wmode wmode);
	void master_mode_w(uint32_t data, te_reg_wmode wmode);

	void execute();
	void illegal_inst();
	uint32_t irp_fetch();
	float irp_fetch_float();

	void add_vertex(const se_vtx &vtx, uint32_t flags);
	void log_triangle(uint32_t flags);
	void setup_triangle(uint32_t flags);
	void calculate_slope(const slope_params &sp, float q1, float q2, float q3, float &slope_out, float &ddx_out);
	void walk_edges(uint32_t wrange);
	void walk_span(pixel_scratch &ps, uint32_t wrange, bool omit_right, uint32_t y, uint32_t xs, uint32_t xe, int32_t r, int32_t g, int32_t b, int32_t a, uint32_t uw, uint32_t vw, uint32_t w,
					uint32_t es_r2l, uint32_t es_ddx_r, uint32_t es_ddx_g, uint32_t es_ddx_b, uint32_t es_ddx_a, uint32_t es_ddx_uw, uint32_t es_ddx_vw, uint32_t es_ddx_w);

	void texcoord_gen(pixel_scratch &ps, const span_regs &rg, uint32_t wrange, uint32_t uw, uint32_t vw, uint32_t w,
						uint32_t & uo, uint32_t & vo, uint32_t & wo);

	uint32_t lod_calc(uint32_t u0, uint32_t v0, uint32_t u1, uint32_t v1);

	uint32_t get_tram_bitdepth(pixel_scratch &ps, const span_regs &rg);

	void addr_calc(pixel_scratch &ps, const span_regs &rg, uint32_t u, uint32_t v, uint32_t lod,
					uint32_t & texaddr, uint32_t & texbit, uint32_t & tdepth);

	void get_texture_color(pixel_scratch &ps, const span_regs &rg, uint32_t u, uint32_t v, uint32_t lod,
							uint32_t & r, uint32_t & g, uint32_t & b, uint32_t & a, uint32_t & s);

	void get_texel(pixel_scratch &ps, const span_regs &rg, uint32_t u, uint32_t v, uint32_t lod,
		uint32_t &r_ti, uint32_t &g_ti, uint32_t &b_ti, uint32_t &a_ti, uint32_t &ssb_ti);

	void texture_fetch(uint32_t texaddr, uint32_t texbit, uint32_t tdepth,
		uint32_t &r_ti, uint32_t &g_ti, uint32_t &b_ti, uint32_t &a_ti, uint32_t &ssb_ti);

	void select_lerp(pixel_scratch &ps, const span_regs &rg, uint32_t sel,
					uint32_t ri, uint32_t gi, uint32_t bi, uint32_t ai,
					uint32_t rt, uint32_t gt, uint32_t bt, uint32_t at, uint32_t ssbt,
					uint32_t & ar, uint32_t & ag, uint32_t & ab );

	void select_mul(pixel_scratch &ps, const span_regs &rg, uint32_t sel, uint32_t ai, uint32_t at, uint32_t ssbt,
					uint32_t & a );

	void texture_blend(pixel_scratch &ps, const span_regs &rg, uint32_t ri, uint32_t gi, uint32_t bi, uint32_t ai,
						uint32_t rt, uint32_t gt, uint32_t bt, uint32_t at, uint32_t ssbt,
						uint32_t &ro, uint32_t &go, uint32_t &bo, uint32_t &ao, uint32_t &ssbo);

	void destination_blend(pixel_scratch &ps, const span_regs &rg, uint32_t x, uint32_t y, uint32_t w, const rgba & ti_color, uint8_t ssb);

	uint8_t color_blend(pixel_scratch &ps, const span_regs &rg, uint8_t ct, uint8_t cti, uint8_t cs, uint8_t csrc,
						uint8_t dm10, uint8_t dm11,
						uint8_t dm20, uint8_t dm21);

	void select_alpha_dsb(pixel_scratch &ps, const span_regs &rg);
	uint8_t get_tex_coef(pixel_scratch &ps, const span_regs &rg, uint8_t cs, uint8_t dm1const0, uint8_t dm1const1);
	uint8_t get_src_coef(pixel_scratch &ps, const span_regs &rg, uint8_t cti, uint8_t dm2const0, uint8_t dm2const1);
	uint8_t dither(uint8_t in, uint8_t dithval);
	uint8_t alu_calc(pixel_scratch &ps, const span_regs &rg, uint16_t a, uint16_t b);
	void select_src_pixel(pixel_scratch &ps, const span_regs &rg);
	void select_tex_pixel(pixel_scratch &ps, const span_regs &rg);
	void write_dst_pixel(pixel_scratch &ps, const span_regs &rg);

	uint8_t read_tram8(offs_t address) const;
	uint16_t read_tram16(offs_t address) const;
	uint32_t read_tram32(offs_t address) const;
	void write_tram8(offs_t address, uint8_t data);
	void write_tram16(offs_t address, uint16_t data);
	void write_tram32(offs_t address, uint32_t data);

	uint8_t read_pipram8(offs_t address) const;
	uint16_t read_pipram16(offs_t address) const;
	uint32_t read_pipram32(offs_t address) const;
	void write_pipram32(offs_t address, uint32_t data);

	uint32_t readbits_from_ram(uint32_t & src_addr, uint32_t & bit_offs, uint32_t bits);
	void load_texture();

	emu_timer           *m_done_timer;
	m2_bda_device       *m_bda;

	devcb_write_line    m_general_int_handler;
	devcb_write_line    m_dfinstr_int_handler;
	devcb_write_line    m_iminstr_int_handler;
	devcb_write_line    m_listend_int_handler;
	devcb_write_line    m_winclip_int_handler;

	// Registers
	union
	{
		struct
		{
			uint32_t te_master_mode;
			uint32_t reserved;
			uint32_t teicntl_data;
			uint32_t teicntl;
			uint32_t tedcntl_data;
			uint32_t tedcntl;
			uint32_t iwp;
			uint32_t irp;
			uint32_t int_enable;
			uint32_t int_status;
			uint32_t vertex_ctrl;
		};
		uint32_t m_regs[11];
	} m_gc;

	union
	{
		struct
		{
			se_vtx  vertices[3];
			uint32_t    reserved[16];
			uint32_t    vertex_state;
		};
		uint32_t m_regs[65];
	} m_se;

	struct
	{
		union
		{
			struct
			{
				uint32_t es_cntl;
				uint32_t es_capaddr;
				uint32_t es_capdata;
			};
			uint32_t m_regs[3];
		};
		union
		{
			struct
			{
				uint32_t x1;
				uint32_t y1;
				uint32_t r1;
				uint32_t g1;
				uint32_t b1;
				uint32_t a1;
				uint32_t w1;
				uint32_t uw1;
				uint32_t vw1;
				uint32_t x2;
				uint32_t y2;
				uint32_t y3;
				uint32_t xstep_0;
				uint32_t xstep_1;
				uint32_t xstep_long;
				uint32_t xystep_0;
				uint32_t xystep_1;
				uint32_t xystep_long;
				uint32_t dy_0;
				uint32_t dy_1;
				uint32_t dy_long;
				uint32_t ddx_r;
				uint32_t ddx_g;
				uint32_t ddx_b;
				uint32_t ddx_a;
				uint32_t ddx_w;
				uint32_t ddx_uw;
				uint32_t ddx_vw;
				uint32_t slope_r;
				uint32_t slope_g;
				uint32_t slope_b;
				uint32_t slope_a;
				uint32_t slope_uw;
				uint32_t slope_vw;
				uint32_t slope_w;
				uint32_t r2l;
			};
			uint32_t m_buffer_regs[36];
		};
	} m_es;

	union
	{
		struct
		{
			uint32_t tex_cntl;
			uint32_t texld_cntl;
			uint32_t tex_addr_cntl;
			uint32_t tex_pip_cntl;
			uint32_t tex_tab_cntl;
			union
			{
				uint32_t tex_lod_base0;
				uint32_t texld_dstbase;
				uint32_t tex_mm_dstbase;
			};
			uint32_t tex_lod_base1;
			uint32_t tex_lod_base2;
			uint32_t tex_lod_base3;
			union
			{
				uint32_t texld_srcbase;
				uint32_t tex_mm_srcbase;
			};
			union
			{
				uint32_t tex_bytecnt;
				uint32_t tex_rowcnt;
				uint32_t tex_texcnt;
			};
			uint32_t uv_max;
			uint32_t uv_mask;
			uint32_t tex_srctype01;
			uint32_t tex_srctype23;
			uint32_t tex_exptype;
			union
			{
				uint32_t tex_srcconst0;
				uint32_t tex_clrconst;
				uint32_t tex_catconst0;
			};
			union
			{
				uint32_t tex_srcconst1;
				uint32_t tex_pipconst1;
			};
			union
			{
				uint32_t tex_srcconst2;
				uint32_t tex_catiConst0;
			};
			union
			{
				uint32_t tex_srcconst3;
				uint32_t tex_caticonst1;
			};
			uint32_t tex_srcexp;
		};
		uint32_t m_regs[21];
	} m_tm;

	union
	{
		struct
		{
			uint32_t snoop;
			uint32_t supergen_ctrl;
			uint32_t usergen_ctrl;
			uint32_t discard_ctrl;
			uint32_t status;
			uint32_t int_ctrl;
			uint32_t fbclip;
			uint32_t x_winclip;
			uint32_t y_winclip;
			uint32_t dst_ctrl;
			uint32_t dst_baseaddr;
			uint32_t dst_xstride;
			uint32_t src_ctrl;
			uint32_t src_baseaddr;
			uint32_t src_xstride;
			uint32_t src_offset;
			uint32_t z_ctrl;
			uint32_t z_baseaddr;
			uint32_t z_offset;
			uint32_t z_clip;
			uint32_t ssbdsb_ctrl;
			uint32_t const_in;
			uint32_t txt_mult_cntl;
			uint32_t txt_coef_const0;
			uint32_t txt_coef_const1;
			uint32_t src_mult_cntl;
			uint32_t src_coef_const0;
			uint32_t src_coef_const1;
			uint32_t alu_ctrl;
			uint32_t src_alpha_ctrl;
			uint32_t dst_alpha_ctrl;
			uint32_t dst_alpha_const;
			uint32_t dither_mat_a;
			uint32_t dither_mat_b;
		};
		uint32_t m_regs[34];
	} m_db;

	te_state    m_state;

	std::unique_ptr<uint32_t[]> m_pipram;
	std::unique_ptr<uint32_t[]> m_tram;

	// Definition of the forward-declared render_snapshot (see its forward
	// declaration above, near pixel_scratch, for what it's for). Defined
	// here since it captures m_gc/m_es/m_tm/m_db's types via decltype.
	struct texture_snapshot
	{
		std::array<uint32_t, PIP_RAM_WORDS> pipram;
		std::array<uint32_t, TEXTURE_RAM_WORDS> tram;
	};

	struct render_snapshot
	{
		decltype(m_gc) gc;
		decltype(m_es) es;
		decltype(m_tm) tm;
		decltype(m_db) db;
		std::shared_ptr<const texture_snapshot> tex;
		ss_surface *dst_surf = nullptr;
		ss_surface *z_surf = nullptr;
		ss_surface *src_surf = nullptr;
	};

	// Register state the per-pixel functions read, copied per span from a
	// render_snapshot - see walk_span()
	struct span_regs
	{
		decltype(m_gc) gc;
		decltype(m_es) es;
		decltype(m_tm) tm;
		decltype(m_db) db;
		const uint32_t *tram;
		const uint32_t *pipram;
		ss_surface *dst_surf;
		ss_surface *z_surf;
		ss_surface *src_surf;
		uint32_t ss_shift;
	};

	// Most-recently-taken snapshot and whether a register write has touched
	// any of render_snapshot's fields since it was taken. queue_span_job()
	// takes a fresh snapshot (copying the current live m_gc/m_es/m_tm/m_db/
	// m_pipram/m_tram) the first time it's called after the dirty flag is
	// set, and reuses it for every subsequent job until the next write().
	// This is what lets write() mutate the live members immediately, without
	// waiting for already-queued jobs to finish rendering first - they hold
	// their own shared_ptr to the snapshot that was current when they were
	// queued, so they're immune to the live members changing under them.
	std::shared_ptr<const render_snapshot> m_cur_snapshot;
	bool m_snapshot_dirty = true;

	// Texture RAM and PIP RAM are copied into a snapshot of their own, only
	// when load_texture() or a PIP RAM register write has changed them, and
	// shared by every register snapshot taken until the next change.
	std::shared_ptr<const texture_snapshot> m_cur_tex;
	bool m_tex_dirty = true;

	// Multi-threaded rasterization: queued-but-not-yet-rendered scanlines,
	// bucketed by band (see span_job above), and the OSD work queue used to
	// render each band's jobs on a worker thread. Allocated once in
	// device_start(). Jobs may now span multiple register-write generations
	// (each tagged with its own render_snapshot - see above), so unlike
	// before, flush_span_jobs() is only called at genuine synchronization
	// points (list end and the safety net at the end of execute()), not
	// before every INST_WRITE_REG - see execute().
	std::vector<span_job> m_span_jobs[NUM_RENDER_BANDS];
	band_state m_bands[NUM_RENDER_BANDS];
	uint32_t m_staged_jobs = 0;

	// Staged jobs are handed to the workers once this many have built up, so
	// rasterization overlaps triangle setup instead of starting at list end.
	static constexpr uint32_t PUBLISH_JOB_COUNT = 128;

	// Several queues rather than one: the OSD caps a WORK_QUEUE_FLAG_HIGH_FREQ
	// queue at min(cores, 4) - 1 = 3 worker threads, so a single queue left
	// most of the host's cores idle while the main thread waited. Bands are
	// spread round-robin across the queues (see flush_span_jobs()).
	static constexpr int NUM_RENDER_QUEUES = 4;
	osd_work_queue *m_render_queue[NUM_RENDER_QUEUES] = { };
};

DECLARE_DEVICE_TYPE(M2_TE, m2_te_device)

#endif // MAME_KONAMI_3DOM2_TE_H
