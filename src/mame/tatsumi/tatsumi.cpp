// license:BSD-3-Clause
// copyright-holders:Bryan McPhail, Angelo Salese
/***************************************************************************

    This is a common base for apache3.cpp, roundup5.cpp and cyclwarr.cpp

    Gray Out (from 1987) is likely a similar setup
    http://www.tatsu-mi.co.jp/game/trace/index.html

    Incredibly complex hardware!  These are all different boards, but share
    a similar sprite chip (TZB215 on Apache 3, TZB315 on others).  Other
    graphics (road, sky, bg/fg layers) all differ between games.

    TODO:
    - Sprite rotation
    - Split these games into individual drivers, write new devices for video routines.
    - Dip switches
    - Deviceify HD6445 (superset of 6845)
    - Various other things..

***************************************************************************/

#include "emu.h"
#include "tatsumi.h"

#include "cpu/nec/nec.h"
#include "cpu/z80/z80.h"
#include "machine/adc0808.h"
#include "machine/i8255.h"
#include "machine/nvram.h"

#include "screen.h"
#include "speaker.h"

#include "endianness.h"


// The games program the HD6445 for 50 x 35 characters of 8 x 8 pixels (400 x 280 total) with 40 x 30
// displayed (320 x 240), so the screen is reconfigured by the CRTC to match. The pixel clock is the
// 50 MHz clock divided by 8, and the CRTC counts characters of 8 pixels. The sprite chip appears to draw
// from a copy of sprite RAM and its control registers taken at the start of vblank.
void tatsumi_state::tatsumi_video(machine_config &config, screen_device &screen)
{
	screen.screen_vblank().append(m_spriteram, FUNC(buffered_spriteram16_device::vblank_copy_rising));
	screen.screen_vblank().append(m_sprite_control_ram, FUNC(buffered_spriteram16_device::vblank_copy_rising));

	HD6345(config, m_crtc, CLOCK_2 / 8 / 8);
	m_crtc->set_screen(screen.tag());
	m_crtc->set_show_border_area(false);
	m_crtc->set_char_width(8);

	BUFFERED_SPRITERAM16(config, m_spriteram);
	BUFFERED_SPRITERAM16(config, m_sprite_control_ram);
}

uint16_t tatsumi_state::tatsumi_sprite_control_r(offs_t offset)
{
	return m_sprite_control_ram->live()[offset];
}

void tatsumi_state::tatsumi_sprite_control_w(offs_t offset, uint16_t data, uint16_t mem_mask)
{
	COMBINE_DATA(&m_sprite_control_ram->live()[offset]);

	/* 0xe0 is bank switch, others unknown */
//  if ((offset==0xe0 && data&0xefff) || offset!=0xe0)
//      logerror("%s:  Tatsumi TZB215 sprite control %04x %08x\n", m_maincpu->pc(), offset, data);
}

// apply shadowing to underlying layers
// TODO: it might mix up with the lower palette bank instead (color bank 0x1400?)
void tatsumi_state::apply_shadow_bitmap(bitmap_rgb32 &bitmap, const rectangle &cliprect, bitmap_ind8 &shadow_bitmap, uint8_t xor_output)
{
	for(int y = cliprect.top(); y <= cliprect.bottom(); y++)
	{
		auto const *const shadow_line = &shadow_bitmap.pix(y);
		auto *const dst = &bitmap.pix(y);
		for(int x = cliprect.left(); x <= cliprect.right(); x++)
		{
			uint8_t shadow = shadow_line[x];
			// xor_output is enabled during Chen boss fight (where shadows have more brightness than everything else)
			// TODO: transition before fighting him should also black out all the background tilemaps too!?
			//       (more evidence that we need to mix with color bank 0x1400 instead of doing true RGB mixing).
			if(shadow ^ xor_output)
			{
				rgb_t shadow_pen = dst[x];
				dst[x] = rgb_t(shadow_pen.r() >> 1, shadow_pen.g() >> 1, shadow_pen.b() >> 1);
			}
		}
	}
}

void tatsumi_state::text_w(offs_t offset, uint16_t data, uint16_t mem_mask)
{
	COMBINE_DATA(&m_videoram[offset]);
	m_tx_layer->mark_tile_dirty(offset);
}

TILE_GET_INFO_MEMBER(tatsumi_state::get_text_tile_info)
{
	int tile = m_videoram[tile_index];
	tileinfo.set(0, tile & 0xfff, tile >> 12, 0);
}

void tatsumi_state::tatsumi_reset()
{
	m_last_control = 0;
	m_control_word = 0;

	save_item(NAME(m_last_control));
	save_item(NAME(m_control_word));
}

uint16_t tatsumi_state::tatsumi_v30_68000_r(offs_t offset)
{
	//logerror("%s:68000_r(%04X),cw=%04X\n", m_maincpu->pc(), offset*2, m_control_word);
	/* Read from 68k RAM */
	if ((m_control_word&0x1f)==0x18)
	{
#if 0
		// hack to make roundup 5 boot
		// doesn't seem necessary anymore, left for reference
		if (m_maincpu->pc()==0xec575)
		{
			auto const dst = util::little_endian_cast<uint8_t>(&m_mainregion[0]);
			dst[0xec57a]=0x46;
			dst[0xec57b]=0x46;

			dst[0xfc520]=0x46; //code that stops cpu after coin counter goes mad..
			dst[0xfc521]=0x46;
			dst[0xfc522]=0x46;
			dst[0xfc523]=0x46;
			dst[0xfc524]=0x46;
			dst[0xfc525]=0x46;
		}
#endif

		return m_sharedram[offset & 0x1fff];
	}

	/* Read from 68k ROM */
	offset+=(m_control_word&0x7)*0x8000;

	return m_subregion[offset];
}

void tatsumi_state::tatsumi_v30_68000_w(offs_t offset, uint16_t data, uint16_t mem_mask)
{
	if ((m_control_word&0x1f)!=0x18)
		logerror("68k write in bank %05x\n",m_control_word);

	COMBINE_DATA(&m_sharedram[offset]);
}

/******************************************************************************/

// apache3 & roundup5 vblank irq vector
IRQ_CALLBACK_MEMBER(tatsumi_state::v30_vector_r)
{
	return 0xc8 / 4;
}


/***************************************************************************/



/***************************************************************************/


void tatsumi_state::init_tatsumi()
{
	tatsumi_reset();
}

/***************************************************************************/
