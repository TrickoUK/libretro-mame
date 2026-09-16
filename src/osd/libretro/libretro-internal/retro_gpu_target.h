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
	retro_gpu_target();
	virtual ~retro_gpu_target();

	// non-copyable: owns GPU resources
	retro_gpu_target(const retro_gpu_target &) = delete;
	retro_gpu_target &operator=(const retro_gpu_target &) = delete;

	virtual void begin_frame(int width, int height) override;
	virtual void upload_texture(const uint32_t *rgba_pixels, int width, int height) override;
	virtual void submit_triangle(const osd::gpu_vertex tri[3], bool textured, osd::gpu_blend_mode blend) override;
	virtual void end_frame_and_readback(uint32_t *rgba_out) override;
	virtual void set_clip_rect(int x1, int y1, int x2, int y2) override;
	virtual void yield_context() override;
	virtual void resume_context() override;

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
	// RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO. Two designs were tried here:
	// (1) hold our context for the whole begin_frame()..
	// end_frame_and_readback() span (cheap - one switch per frame), with
	// just the one known risky call site (psxgpu_device::
	// updatevisiblearea()) bracketed via yield_context()/resume_context();
	// (2) even a *lazily re-acquired* whole-frame hold, released at the
	// very end of every readback so nothing is ever bound across a frame
	// boundary we don't control. Both eventually crashed a few frames
	// after a resolution change, with our own context ending up
	// (impossibly, if our bookkeeping were the whole story) left "current"
	// again without an intervening begin_frame() - strongly suggesting
	// this is a Mesa/driver-level issue with RetroArch recreating its own
	// EGL context/surface while ours coexists on the same GPU device, not
	// something fixable purely through EGL call-level bookkeeping on our
	// side. Reverted to the original, narrowest design: every public entry
	// point below (see the scoped_context helper in the .cpp) individually
	// saves/restores around itself, so our context is *only* current for
	// the duration of a single call, at the cost of an eglMakeCurrent pair
	// per call (confirmed stable via an extended soak test; confirmed slow
	// with real per-polygon-heavy scenes - a known, accepted tradeoff for
	// now). yield_context()/resume_context() are consequently no-ops here -
	// see their definitions - kept as overrides only so a future, provably
	// safe wider-hold design doesn't need to touch call sites again.

	uint32_t m_fbo;
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
