// license:BSD-3-Clause
// copyright-holders:TrickoUK
/***************************************************************************

    retro_gpu_target.h

    Headless EGL/OpenGL implementation of osd::gpu_render_target for the
    libretro OSD. Creates its own private GL context, independent of
    whatever context RetroArch's own frontend is using - see CLAUDE.md
    "Chosen first target" for why this architecture was picked over
    reviving libretro's RETRO_HW_RENDER API.

    Only compiled when HAVE_RETRO_GPU_TARGET is defined; retro_osd_interface's
    get_gpu_render_target() returns nullptr otherwise (inherited default
    from osd_common_t).

***************************************************************************/
#ifndef MAME_OSD_LIBRETRO_RETRO_GPU_TARGET_H
#define MAME_OSD_LIBRETRO_RETRO_GPU_TARGET_H

#pragma once

#include "interface/gpurender.h"

#include <cstdint>
#include <vector>

typedef void *EGLDisplay;
typedef void *EGLContext;
typedef void *EGLSurface;

class retro_gpu_target : public osd::gpu_render_target
{
public:
	// msaa_samples: 0 disables MSAA (the FBO still goes through the
	// resolve-blit path unconditionally for one simple code path rather
	// than two - see resize_target()/end_frame_and_readback() - a
	// zero-sample renderbuffer is spec-legal and behaves as a plain
	// single-sample one, so this costs one harmless extra blit per frame
	// when disabled, not a code fork).
	// texfilter_mode: 0 = nearest (PS1-accurate, default), 1 = bilinear,
	// 2 = trilinear (bilinear + a coarser box-filtered blend on minified
	// polygons - see fragment_shader_src in the .cpp for why there's no
	// real mip chain involved), 3 = 3-point/N64-style (barycentric blend
	// of 3 of the 4 texels instead of bilinear's blend of all 4).
	explicit retro_gpu_target(int msaa_samples = 4, int texfilter_mode = 0);
	virtual ~retro_gpu_target();

	// non-copyable: owns GPU resources
	retro_gpu_target(const retro_gpu_target &) = delete;
	retro_gpu_target &operator=(const retro_gpu_target &) = delete;

	virtual void begin_frame(int width, int height) override;
	virtual void upload_vram(const uint16_t *vram_words, int width, int height) override;
	virtual void set_texture_page(int tx, int ty, int tp, int clutx, int cluty) override;
	virtual void submit_triangle(const osd::gpu_vertex tri[3], bool textured, osd::gpu_blend_mode blend, bool filterable = true) override;
	virtual void submit_triangles(const osd::gpu_vertex *verts, int count, bool textured, osd::gpu_blend_mode blend, bool filterable = true) override;
	virtual void end_frame_and_readback(uint32_t *rgba_out) override;
	virtual void set_clip_rect(int x1, int y1, int x2, int y2) override;
	virtual void copy_rect(int sx, int sy, int dx, int dy, int w, int h) override;
	virtual void begin_batch() override;
	virtual void end_batch() override;

	// true once EGL/GL initialization has succeeded; callers (and
	// get_gpu_render_target()) should treat a failed-to-initialize
	// instance the same as no instance at all (return nullptr instead).
	bool is_valid() const { return m_valid; }

private:
	bool init_context();
	void resize_target(int width, int height);
	uint32_t compile_program();
	void set_blend_mode(osd::gpu_blend_mode blend);

	EGLDisplay m_display;
	EGLContext m_context;
	EGLSurface m_surface;
	bool m_valid;

	// The caller (MAME, via psxgpu_device et al) runs on the same thread as
	// RetroArch's own EGL/GL context, and RetroArch's frontend can perform
	// its own EGL calls (eglMakeCurrent/eglSwapInterval, and apparently a
	// full context/surface teardown-and-recreate) synchronously from
	// *inside* a MAME core call - concretely, in reaction to our
	// scaled-resolution screen.configure() call propagating to
	// RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO. Two earlier designs held our
	// context across *multiple* MAME calls (once per frame, or lazily
	// re-acquired and held until the next readback) to avoid per-call
	// eglMakeCurrent overhead, and both eventually crashed a few frames
	// after a resolution change - diagnostics showed our own context
	// ending up "current" again without an intervening begin_frame(),
	// impossible if our own bookkeeping were the whole story, strongly
	// suggesting a Mesa/driver-level issue with RetroArch recreating its
	// own EGL context/surface while ours coexists on the same GPU device
	// *and control has actually returned to MAME/RetroArch's scheduler in
	// between* - not something fixable purely through EGL call-level
	// bookkeeping on our side.
	//
	// Every public entry point below therefore still individually
	// saves/restores around itself by default (see the scoped_context
	// helper in the .cpp) - safe on its own, but an eglMakeCurrent pair
	// per call is too slow for a real polygon-heavy scene. begin_batch()/
	// end_batch() add an opt-in fast path *psxgpu_device actually uses*:
	// the caller queues a whole frame's worth of upload_vram()/set_texture_page()/
	// set_clip_rect()/submit_triangle() calls in host memory (no GL calls
	// at all yet), then replays the queue in one tight, synchronous C++
	// loop bracketed by begin_batch()/end_batch() - during which control
	// never returns to MAME's scheduler or RetroArch, so the risky
	// interleaving above is structurally impossible, not just unlikely.
	// scoped_context checks m_batch_active and skips its own
	// eglMakeCurrent pair when a batch already has the context current -
	// see its 4th constructor argument.
	bool m_batch_active;
	EGLDisplay m_batch_saved_display;
	EGLSurface m_batch_saved_draw_surface;
	EGLSurface m_batch_saved_read_surface;
	EGLContext m_batch_saved_context;

	// m_fbo is the actual render target - a multisample color renderbuffer
	// attachment (MSAA, see MSAA_SAMPLES in the .cpp), not a plain texture.
	// glReadPixels can't read a multisample framebuffer directly, so
	// end_frame_and_readback() resolves m_fbo into m_resolve_fbo (a
	// same-size single-sample FBO with a plain texture attachment,
	// m_color_tex) via glBlitFramebuffer first, then reads back from
	// m_resolve_fbo. copy_rect() (mid-frame VRAM-to-VRAM blits) operates
	// on m_fbo directly and stays multisampled all frame - only the very
	// last step, right before readback, drops to single-sample.
	int m_msaa_samples;

	uint32_t m_fbo;
	uint32_t m_color_rb_ms;
	uint32_t m_resolve_fbo;
	uint32_t m_color_tex;
	int m_fbo_width;
	int m_fbo_height;

	uint32_t m_vram_tex;
	int m_vram_tex_width;
	int m_vram_tex_height;

	uint32_t m_program;
	uint32_t m_vao;
	uint32_t m_vbo;
	int m_u_target_size_loc;
	int m_u_textured_loc;
	int m_u_tp_loc;
	int m_u_tx_loc;
	int m_u_ty_loc;
	int m_u_clutx_loc;
	int m_u_cluty_loc;
	int m_u_vram_height_loc;
	int m_u_texfilter_loc;

	int m_texfilter_mode;

	osd::gpu_blend_mode m_current_blend;

	// Tracked so end_frame_and_readback() can temporarily disable
	// GL_SCISSOR_TEST around its glReadPixels call (which is itself
	// affected by the scissor box - reading back with a restrictive draw
	// area still set would silently only capture that sub-region) and then
	// restore it - the draw area/clip rect is persistent GPU-like state
	// across frames (matches real PS1 hardware), not reset every frame.
	bool m_scissor_enabled;
	int m_scissor_x, m_scissor_y, m_scissor_w, m_scissor_h;
};

#endif // MAME_OSD_LIBRETRO_RETRO_GPU_TARGET_H
