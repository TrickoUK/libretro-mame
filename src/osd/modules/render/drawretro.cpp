
//============================================================
//
//  drawretro.c - basic retro drawing
//
//============================================================

#include "render_module.h"

#include "modules/osdmodule.h"

#include "drawretro.h"
#include "rendersw.hxx"

//============================================================
//  destructor
//============================================================

renderer_retro::~renderer_retro()
{
}

//============================================================
//  renderer_retro::create
//============================================================

int renderer_retro::create()
{
	return 0;
}

//============================================================
//  renderer_retro::get_primitives
//============================================================

render_primitive_list *renderer_retro::get_primitives()
{
	osd_dim nd = window().get_size();
	if (nd != m_blit_dim)
	{
		m_blit_dim = nd;
		notify_changed();
	}
	window().target()->set_bounds(m_blit_dim.width(), m_blit_dim.height(), window().pixel_aspect());
	return &window().target()->get_primitives();
}

int renderer_retro::xy_to_render_target(int x, int y, int *xt, int *yt)
{
	osd_dim nd = window().get_size();

	*xt = x;
	*yt = y;
	if (*xt<0 || *xt >= nd.width())
		return 0;
	if (*yt<0 || *yt >= nd.height())
		return 0;
	return 1;
}

//============================================================
//  renderer_retro::draw
//============================================================

int renderer_retro::draw(const int update)
{
	// get the target bounds
	osd_dim nd = window().get_size();

	// compute width/height/pitch of target
	int width = nd.width();
	int height = nd.height();

	m_bmdata = (uint8_t *)retro_get_fb_ptr();

	//FIXME retro handle 16/32 bits

	// draw the primitives to the bitmap
	window().m_primlist->acquire_lock();
	software_renderer<uint32_t, 0,0,0, 16,8,0>::draw_primitives(*window().m_primlist, m_bmdata, width, height, width);
	window().m_primlist->release_lock();

	return 0;
}

namespace osd {

class video_retro : public osd_module, public render_module
{
public:
	video_retro()
		: osd_module(OSD_RENDERER_PROVIDER, "soft")
	{
	}

	virtual int init(osd_interface &osd, osd_options const &options) override;
	virtual void exit() override {};

	virtual std::unique_ptr<osd_renderer> create(osd_window &window) override;

protected:
	virtual unsigned flags() const override { return FLAG_INTERACTIVE; }

private:
};

int video_retro::init(osd_interface &osd, osd_options const &options)
{
	return 0;
}

std::unique_ptr<osd_renderer> video_retro::create(osd_window &window)
{
	return std::make_unique<renderer_retro>(window);
}

} // namespace osd

MODULE_DEFINITION(RENDERER_RETRO, osd::video_retro)
