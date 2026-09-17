// license:BSD-3-Clause
// copyright-holders:smf
/*
 * PlayStation GPU emulator
 *
 * Copyright 2003-2014 smf
 *
 */

#ifndef MAME_VIDEO_PSX_H
#define MAME_VIDEO_PSX_H

#pragma once

#include <vector>

// GPU-accelerated rendering (see CLAUDE.md "Chosen first target" /
// /home/bazzite/.claude/plans/glowing-conjuring-raven.md Phase 2).
// gpu_queued_cmd (below) embeds osd::gpu_vertex/gpu_blend_mode by value, so
// this needs the full interface, not just a forward declaration of
// osd::gpu_render_target - still a small, OSD-agnostic header (no EGL/GL),
// so no build-time cost for OSDs/builds without the feature.
// osd_interface::get_gpu_render_target() (declared in osdepend.h, which
// psx.cpp includes) returns nullptr when unavailable, so m_gpu_render_target
// below is always safe to store either way.
#include "interface/gpurender.h"

#define PSXGPU_DEBUG_VIEWER ( 0 )

DECLARE_DEVICE_TYPE(CXD8514Q,  cxd8514q_device)
DECLARE_DEVICE_TYPE(CXD8538Q,  cxd8538q_device)
DECLARE_DEVICE_TYPE(CXD8561Q,  cxd8561q_device)
DECLARE_DEVICE_TYPE(CXD8561BQ, cxd8561bq_device)
DECLARE_DEVICE_TYPE(CXD8561CQ, cxd8561cq_device)
DECLARE_DEVICE_TYPE(CXD8654Q,  cxd8654q_device)

class psxcpu_device;

class psxgpu_device : public device_t, public device_video_interface, public device_palette_interface
{
public:
	// configuration helpers
	auto vblank_callback() { return m_vblank_handler.bind(); }
	int vram_size() { return vramSize; }

	void write(offs_t offset, uint32_t data, uint32_t mem_mask = ~0);
	uint32_t read(offs_t offset, uint32_t mem_mask = ~0);
	void dma_read( uint32_t *ram, uint32_t n_address, int32_t n_size );
	void dma_write( uint32_t *ram, uint32_t n_address, int32_t n_size );
	void lightgun_set( int, int );

	static constexpr feature_type imperfect_features() { return feature::GRAPHICS; }

protected:
	static constexpr unsigned MAX_LEVEL = 32;
	static constexpr unsigned MID_LEVEL = (MAX_LEVEL / 2) << 8;
	static constexpr unsigned MAX_SHADE = 0x100;
	static constexpr unsigned MID_SHADE = 0x80;

	// construction/destruction
	psxgpu_device(const machine_config &mconfig, device_type type, const char *tag, device_t *owner, uint32_t clock, uint32_t vram_size, psxcpu_device *cpu_tag);
	psxgpu_device(const machine_config &mconfig, device_type type, const char *tag, device_t *owner, uint32_t clock);

	virtual void device_start() override ATTR_COLD;
	virtual void device_post_load() override;
	virtual void device_reset() override ATTR_COLD;
	virtual void device_config_complete() override;

	// device_palette_interface overrides
	virtual uint32_t palette_entries() const noexcept override { return 32*32*32*2; }

	int vramSize;

private:
	static constexpr unsigned DEBUG_COORDS = 10;

	struct psx_gpu_debug
	{
		std::unique_ptr<bitmap_ind16> mesh;
		int b_clear;
		int b_mesh;
		int n_skip;
		int b_texture;
		int n_interleave;
		int n_coord;
		int n_coordx[ DEBUG_COORDS ];
		int n_coordy[ DEBUG_COORDS ];
	};

	struct FLATVERTEX
	{
		PAIR n_coord;
	};

	struct GOURAUDVERTEX
	{
		PAIR n_bgr;
		PAIR n_coord;
	};

	struct FLATTEXTUREDVERTEX
	{
		PAIR n_coord;
		PAIR n_texture;
	};

	struct GOURAUDTEXTUREDVERTEX
	{
		PAIR n_bgr;
		PAIR n_coord;
		PAIR n_texture;
	};

	union PACKET
	{
		uint32_t n_entry[ 16 ];

		struct
		{
			PAIR n_cmd;
			struct FLATVERTEX vertex[ 2 ];
			PAIR n_size;
		} MoveImage;

		struct
		{
			PAIR n_bgr;
			PAIR n_coord;
			PAIR n_size;
		} FlatRectangle;

		struct
		{
			PAIR n_bgr;
			PAIR n_coord;
		} FlatRectangle8x8;

		struct
		{
			PAIR n_bgr;
			PAIR n_coord;
		} FlatRectangle16x16;

		struct
		{
			PAIR n_bgr;
			PAIR n_coord;
			PAIR n_texture;
		} Sprite8x8;

		struct
		{
			PAIR n_bgr;
			PAIR n_coord;
			PAIR n_texture;
		} Sprite16x16;

		struct
		{
			PAIR n_bgr;
			PAIR n_coord;
			PAIR n_texture;
			PAIR n_size;
		} FlatTexturedRectangle;

		struct
		{
			PAIR n_bgr;
			struct FLATVERTEX vertex[ 4 ];
		} FlatPolygon;

		struct
		{
			struct GOURAUDVERTEX vertex[ 4 ];
		} GouraudPolygon;

		struct
		{
			PAIR n_bgr;
			struct FLATVERTEX vertex[ 2 ];
		} MonochromeLine;

		struct
		{
			struct GOURAUDVERTEX vertex[ 2 ];
		} GouraudLine;

		struct
		{
			PAIR n_bgr;
			struct FLATTEXTUREDVERTEX vertex[ 4 ];
		} FlatTexturedPolygon;

		struct
		{
			struct GOURAUDTEXTUREDVERTEX vertex[ 4 ];
		} GouraudTexturedPolygon;

		struct
		{
			PAIR n_bgr;
			struct FLATVERTEX vertex;
		} Dot;

		struct
		{
			PAIR n_bgr;
			struct FLATTEXTUREDVERTEX vertex;
		} TexturedDot;
	};

	void updatevisiblearea();
	void decode_tpage( uint32_t tpage );
	void FlatPolygon( int n_points );
	void FlatTexturedPolygon( int n_points );
	void GouraudPolygon( int n_points );
	void GouraudTexturedPolygon( int n_points );
	void MonochromeLine();
	void GouraudLine();
	void FrameBufferRectangleDraw();
	void FlatRectangle();
	void FlatRectangle8x8();
	void FlatRectangle16x16();
	void FlatTexturedRectangle();
	void Sprite8x8();
	void Sprite16x16();
	void Dot();
	void TexturedDot();
	void MoveImage();
	void psx_gpu_init( int n_gputype );
	void gpu_reset();
	void gpu_read( uint32_t *p_ram, int32_t n_size );
	void gpu_write( uint32_t *p_ram, int32_t n_size );

	// GPU-accelerated rendering (Phase 2): only the four polygon primitives
	// are routed to the GPU; lines/rects/sprites/dots/framebuffer-copy
	// commands still use the original software VRAM path (see CLAUDE.md).
	// Lazily acquires the GPU render target on first call (see psx.cpp) -
	// deliberately NOT done in device_start()/device_reset(), which run
	// too early (before RetroArch sets up its own graphics context; see
	// the comment in device_start()). Not const because of the lazy init.
	bool gpu_active();
	bool m_gpu_active_checked = false;

	// Per-call EGL context acquisition (one eglMakeCurrent pair per
	// set_texture_page()/submit_triangle()/set_clip_rect() call) was too slow
	// for a real polygon-heavy scene. Rather than holding the context
	// across multiple calls to amortize that cost - which was tried and
	// found to eventually corrupt shared driver state once control returns
	// to MAME/RetroArch's scheduler in between (see CLAUDE.md/plan file
	// Phase 2 status) - primitive submission is deferred: gpu_submit_
	// triangle_pair()/gpu_maybe_set_texture_page()/gpu_maybe_set_clip_
	// rect() queue commands here instead of calling m_gpu_render_target
	// directly. gpu_update_screen() then replays the whole queue in one
	// tight, synchronous loop bracketed by begin_batch()/end_batch() -
	// control never returns to the scheduler during that loop, so the
	// risky interleaving is structurally impossible, not just unlikely,
	// while still paying only one context acquisition for the whole frame.
	//
	// A structured list (not opaque closures) so gpu_update_screen() can
	// also merge consecutive TRIANGLE commands that share the same
	// textured/blend state into one larger osd::gpu_render_target::
	// submit_triangles() call instead of replaying them one triangle (one
	// GL draw call) at a time - per-draw-call overhead, independent of the
	// EGL context-switching problem above, is itself significant for a
	// real polygon-heavy scene.
	struct gpu_queued_cmd
	{
		enum class kind_t { TRIANGLE, TEXPARAM, CLIP, COPY } kind;
		osd::gpu_vertex tri[3];
		bool textured = false;
		osd::gpu_blend_mode blend = osd::gpu_blend_mode::NONE;
		int tex_tx = 0, tex_ty = 0, tex_tp = 0, tex_clutx = 0, tex_cluty = 0;
		int clip_x1 = 0, clip_y1 = 0, clip_x2 = 0, clip_y2 = 0;
		int copy_sx = 0, copy_sy = 0, copy_dx = 0, copy_dy = 0, copy_w = 0, copy_h = 0;
	};
	std::vector<gpu_queued_cmd> m_gpu_queue;
	osd::gpu_blend_mode gpu_blend_mode_for( uint8_t n_cmd ) const;
	void gpu_queue_triangle_pair( const osd::gpu_vertex &v0, const osd::gpu_vertex &v1, const osd::gpu_vertex &v2, const osd::gpu_vertex &v3, int n_points, bool textured, osd::gpu_blend_mode blend );
	void gpu_submit_triangle_pair( const osd::gpu_vertex &v0, const osd::gpu_vertex &v1, const osd::gpu_vertex &v2, const osd::gpu_vertex &v3, int n_points, bool textured, osd::gpu_blend_mode blend );
	void gpu_force_no_clip();
	bool gpu_submit_flat_polygon( int n_points );
	bool gpu_submit_flat_textured_polygon( int n_points );
	bool gpu_submit_gouraud_polygon( int n_points );
	bool gpu_submit_gouraud_textured_polygon( int n_points );
	bool gpu_submit_flat_rectangle( int32_t n_x, int32_t n_y, int32_t n_w, int32_t n_h, PAIR n_bgr );
	bool gpu_submit_textured_rectangle( int32_t n_x, int32_t n_y, int32_t n_w, int32_t n_h, uint8_t n_u0, uint8_t n_v0, PAIR n_bgr, int32_t n_tx, int32_t n_ty, int32_t n_tp, uint32_t n_clutx, uint32_t n_cluty );
	bool gpu_submit_vram_fill_rectangle( int32_t n_x, int32_t n_y, int32_t n_w, int32_t n_h, PAIR n_bgr );
	bool gpu_submit_image_stamp( int32_t n_x, int32_t n_y, int32_t n_w, int32_t n_h );
	bool gpu_submit_dot( int32_t n_x, int32_t n_y, PAIR n_bgr );
	bool gpu_submit_textured_dot( int32_t n_x, int32_t n_y, uint8_t n_u0, uint8_t n_v0, PAIR n_bgr, int32_t n_tx, int32_t n_ty, int32_t n_tp, uint32_t n_clutx, uint32_t n_cluty );
	bool gpu_submit_line( int32_t n_x0, int32_t n_y0, int32_t n_x1, int32_t n_y1, float r0, float g0, float b0, float r1, float g1, float b1, uint8_t n_cmd );
	bool gpu_queue_copy_rect( int32_t n_sx, int32_t n_sy, int32_t n_dx, int32_t n_dy, int32_t n_w, int32_t n_h );
	uint32_t gpu_update_screen( bitmap_rgb32 &bitmap );

	osd::gpu_render_target *m_gpu_render_target = nullptr;

	// The user-selectable internal-resolution multiplier (mame_psx_gpu_hle
	// core option on the retro OSD - "disabled"/"2x"/"4x", read once at
	// boot; see osdepend.h's gpu_render_scale() and CLAUDE.md "Chosen first
	// target"). A cheap query (just returns a stored int on the retro OSD,
	// no GPU context work), safe to call anywhere gpu_render_available() is
	// also safe to call, including before gpu_active() has ever run.
	// Defined in psx.cpp, not inline here - osd_interface (returned by
	// machine().osd()) is only forward-declared where psx.h gets included
	// by other drivers, not fully defined.
	int gpu_scale() const;

	// Avoids re-issuing a set_texture_page() GL state change on every single
	// textured polygon when consecutive polygons share one texture page -
	// see gpu_maybe_set_texture_page() in psx.cpp. -1 means "nothing set
	// yet this session", guaranteed to mismatch any real n_tx/n_ty (always
	// >= 0). Texture *data* itself is no longer decoded/cached here at all -
	// upload_vram() in gpu_update_screen() uploads the device's whole raw
	// VRAM once per frame and the GPU shader decodes CLUT/bpp addressing
	// directly from it (see retro_gpu_target's fragment shader) - this
	// cache is now purely about skipping a handful of uniform updates.
	int32_t m_gpu_last_tx = -1;
	int32_t m_gpu_last_ty = -1;
	int32_t m_gpu_last_tp = -1;
	int32_t m_gpu_last_clutx = -1;
	int32_t m_gpu_last_cluty = -1;
	void gpu_maybe_set_texture_page( int n_tx, int n_ty, int tp, int n_clutx, int n_cluty );

	// Avoids calling set_clip_rect() (a GL scissor-state change) on every
	// polygon when consecutive polygons share the same PS1 draw area,
	// mirroring the texture-page cache above. false until the first
	// polygon submission each session, guaranteeing the first call always
	// applies whatever draw area is current at that point.
	bool m_gpu_drawarea_cached = false;
	uint32_t m_gpu_last_drawarea_x1 = 0;
	uint32_t m_gpu_last_drawarea_y1 = 0;
	uint32_t m_gpu_last_drawarea_x2 = 0;
	uint32_t m_gpu_last_drawarea_y2 = 0;
	void gpu_maybe_set_clip_rect();

	int32_t m_n_tx;
	int32_t m_n_ty;
	int32_t n_abr;
	int32_t n_tp;
	int32_t n_ix;
	int32_t n_iy;
	int32_t n_ti;

	std::unique_ptr<uint16_t[]> p_vram;
	// Row count of p_vram (width is always 1024 words) - passed to
	// osd::gpu_render_target::upload_vram() so its shader can wrap texture-
	// page row addressing by the actual VRAM height, exactly like
	// p_p_vram's row lookup table already does for the CPU path (see
	// psx_gpu_init()).
	int m_vram_height = 0;
	uint32_t n_vramx;
	uint32_t n_vramy;
	uint32_t n_twy;
	uint32_t n_twx;
	uint32_t n_twh;
	uint32_t n_tww;
	uint32_t n_drawarea_x1;
	uint32_t n_drawarea_y1;
	uint32_t n_drawarea_x2;
	uint32_t n_drawarea_y2;
	uint32_t n_horiz_disstart;
	uint32_t n_horiz_disend;
	uint32_t n_vert_disstart;
	uint32_t n_vert_disend;
	uint32_t b_reverseflag;
	int32_t n_drawoffset_x;
	int32_t n_drawoffset_y;
	uint32_t m_n_displaystartx;
	uint32_t n_displaystarty;
	int m_n_gputype;
	uint32_t n_gpustatus;
	uint32_t n_gpuinfo;
	uint32_t n_gpu_buffer_offset;
	uint32_t n_lightgun_x;
	uint32_t n_lightgun_y;
	uint32_t n_screenwidth;
	uint32_t n_screenheight;
	bool m_draw_stp;
	bool m_check_stp;

	PACKET m_packet;

	uint16_t *p_p_vram[ 1024 ];

	uint16_t p_n_redshade[ MAX_LEVEL * MAX_SHADE ];
	uint16_t p_n_greenshade[ MAX_LEVEL * MAX_SHADE ];
	uint16_t p_n_blueshade[ MAX_LEVEL * MAX_SHADE ];
	uint16_t p_n_redlevel[ 0x10000 ];
	uint16_t p_n_greenlevel[ 0x10000 ];
	uint16_t p_n_bluelevel[ 0x10000 ];

	uint16_t p_n_f025[ MAX_LEVEL * MAX_SHADE ];
	uint16_t p_n_f05[ MAX_LEVEL * MAX_SHADE ];
	uint16_t p_n_f1[ MAX_LEVEL * MAX_SHADE ];
	uint16_t p_n_redb05[ 0x10000 ];
	uint16_t p_n_greenb05[ 0x10000 ];
	uint16_t p_n_blueb05[ 0x10000 ];
	uint16_t p_n_redb1[ 0x10000 ];
	uint16_t p_n_greenb1[ 0x10000 ];
	uint16_t p_n_blueb1[ 0x10000 ];
	uint16_t p_n_redaddtrans[ MAX_LEVEL * MAX_LEVEL ];
	uint16_t p_n_greenaddtrans[ MAX_LEVEL * MAX_LEVEL ];
	uint16_t p_n_blueaddtrans[ MAX_LEVEL * MAX_LEVEL ];
	uint16_t p_n_redsubtrans[ MAX_LEVEL * MAX_LEVEL ];
	uint16_t p_n_greensubtrans[ MAX_LEVEL * MAX_LEVEL ];
	uint16_t p_n_bluesubtrans[ MAX_LEVEL * MAX_LEVEL ];

	uint32_t p_n_g0r0[ 0x10000 ];
	uint32_t p_n_b0[ 0x10000 ];
	uint32_t p_n_r1[ 0x10000 ];
	uint32_t p_n_b1g1[ 0x10000 ];

	devcb_write_line m_vblank_handler;

	void vblank(screen_device &screen, bool vblank_state);
	uint32_t update_screen(screen_device &screen, bitmap_rgb32 &bitmap, const rectangle &cliprect);

#if defined(PSXGPU_DEBUG_VIEWER) && PSXGPU_DEBUG_VIEWER
	void DebugMeshInit();
	void DebugMesh( int n_coordx, int n_coordy );
	void DebugMeshEnd();
	void DebugCheckKeys();
	int DebugMeshDisplay( bitmap_rgb32 &bitmap, const rectangle &cliprect );
	int DebugTextureDisplay( bitmap_rgb32 &bitmap );

	psx_gpu_debug m_debug;
#endif
};

class cxd8514q_device : public psxgpu_device
{
public:
	// construction/destruction
	cxd8514q_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock, uint32_t vram_size, psxcpu_device *cpu);
	cxd8514q_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock);
};

class cxd8538q_device : public psxgpu_device
{
public:
	// construction/destruction
	cxd8538q_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock, uint32_t vram_size, psxcpu_device *cpu);
	cxd8538q_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock);
};

class cxd8561q_device : public psxgpu_device
{
public:
	// construction/destruction
	cxd8561q_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock, uint32_t vram_size, psxcpu_device *cpu);
	cxd8561q_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock);
};

class cxd8561bq_device : public psxgpu_device
{
public:
	// construction/destruction
	cxd8561bq_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock, uint32_t vram_size, psxcpu_device *cpu);
	cxd8561bq_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock);
};

class cxd8561cq_device : public psxgpu_device
{
public:
	// construction/destruction
	cxd8561cq_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock, uint32_t vram_size, psxcpu_device *cpu);
	cxd8561cq_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock);
};

class cxd8654q_device : public psxgpu_device
{
public:
	// construction/destruction
	cxd8654q_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock, uint32_t vram_size, psxcpu_device *cpu);
	cxd8654q_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock);
};

#endif // MAME_VIDEO_PSX_H
