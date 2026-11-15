
//============================================================
//
//  drawretro.h - basic retro drawing
//
//============================================================

#pragma once

#ifndef __DRAWRETRO__
#define __DRAWRETRO__

// OSD Headers
#include "osdretro.h"

// MAME headers
#include "emucore.h"
#include "render.h"

// MAMEOS headers
#include "window.h"


//============================================================
//  TYPE DEFINITIONS
//============================================================

class renderer_retro : public osd_renderer
{
public:
	renderer_retro(osd_window &window)
		: osd_renderer(window)
		, m_blit_dim(0, 0)
		, m_last_dim(0, 0)
		, m_bmdata(nullptr)
		, m_bmsize(0)
	{
	}
	virtual ~renderer_retro();

	static bool init(running_machine &machine) { return false; }
	static void exit() { }

	virtual int create() override;
	virtual int draw(const int update) override;
	virtual void save() override {};
	virtual void record() override {};
	virtual void toggle_fsfx() override {};
	virtual int xy_to_render_target(const int x, const int y, int *xt, int *yt) override;
	virtual render_primitive_list *get_primitives() override;

private:
	osd_dim             m_blit_dim;
	osd_dim             m_last_dim;

	uint8_t *               m_bmdata;
	size_t                  m_bmsize;
};

#endif // __DRAWRETRO__
