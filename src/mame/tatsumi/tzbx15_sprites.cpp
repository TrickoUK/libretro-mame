// license:BSD-3-Clause
// copyright-holders:Bryan McPhail, Angelo Salese

#include "emu.h"
#include "tzbx15_sprites.h"
#include "screen.h"

#include <numbers>

// TZB215 on Apache 3
// TZB315 on Round Up 5, Big Fight, Cycle Warriors
// differences, if any, unknown
// (does not appear to be CLUT size, even if that would have made sense.
//  Round Up 5 uses the smaller CLUT like Apache 3 yet is confirmed to be a TZB315)

DEFINE_DEVICE_TYPE(TZB215_SPRITES, tzb215_device, "tzb215_sprites", "Tatsumi TZB215 Rotating Sprites")
DEFINE_DEVICE_TYPE(TZB315_SPRITES, tzb315_device, "tzb315_sprites", "Tatsumi TZB315 Rotating Sprites")

tzbx15_device::tzbx15_device(const machine_config &mconfig, device_type type, const char *tag, device_t *owner, u32 clock)
	: device_t(mconfig, type, tag, owner, clock)
	, device_gfx_interface(mconfig, *this)
	, m_palette_clut(*this, "palette_clut")
	, m_palette_base(*this, finder_base::DUMMY_TAG)
	, m_spriteram(*this, finder_base::DUMMY_TAG)
	, m_sprites_l_rom(*this, "sprites_l")
	, m_sprites_h_rom(*this, "sprites_h")
{
}

tzbx15_device::tzbx15_device(const machine_config &mconfig, device_type type, const char *tag, device_t *owner, u32 clock, u32 clut_size)
	: tzbx15_device(mconfig, type, tag, owner, clock)
{
	m_rom_clut_size = clut_size;
}

tzb215_device::tzb215_device(const machine_config &mconfig, const char *tag, device_t *owner, u32 clock, u32 clut_size)
	: tzbx15_device(mconfig, TZB215_SPRITES, tag, owner, clock, clut_size)
{
}

tzb215_device::tzb215_device(const machine_config &mconfig, const char *tag, device_t *owner, u32 clock)
	: tzbx15_device(mconfig, TZB215_SPRITES, tag, owner, clock, 0)
{
}

tzb315_device::tzb315_device(const machine_config &mconfig, const char *tag, device_t *owner, u32 clock, u32 clut_size)
	: tzbx15_device(mconfig, TZB315_SPRITES, tag, owner, clock, clut_size)
{
}

tzb315_device::tzb315_device(const machine_config &mconfig, const char *tag, device_t *owner, u32 clock)
	: tzbx15_device(mconfig, TZB315_SPRITES, tag, owner, clock, 0)
{
}




static const gfx_layout spritelayout =
{
	8,8,
	RGN_FRAC(1,1),
	4,
	{ STEP4(0,1) },
	{ 8,12,0,4, 24,28,16,20 },
	{ STEP8(0,4*8) },
	32*8
};

GFXDECODE_MEMBER( tzbx15_device::gfxinfo )
	GFXDECODE_DEVICE("sprites_l", 0, spritelayout, 0, 256)
	GFXDECODE_DEVICE("sprites_h", 0, spritelayout, 0, 256)
GFXDECODE_END

void tzbx15_device::device_start()
{
	m_rom_clut_offset = memregion("sprites_l")->bytes() - m_rom_clut_size;

	m_shadow_pen_array = make_unique_clear<uint8_t[]>(m_rom_clut_size * 2);
	m_rot_bitmap.allocate(ROT_ORIGIN * 2, ROT_ORIGIN * 2);
	m_rot_bitmap.fill(0);
	m_rot_bounds = EMPTY_BOUNDS;

	decode_gfx(gfxinfo);
	gfx(0)->set_colors(m_rom_clut_size / 8);
	gfx(1)->set_colors(m_rom_clut_size / 8);
}

void tzbx15_device::device_reset()
{
}

void tzbx15_device::device_add_mconfig(machine_config &config)
{
	// 4096 or 8192 arranged as series of CLUTs
	PALETTE(config, m_palette_clut).set_format(palette_device::xRGB_555, m_rom_clut_size * 2);
}

template<class BitmapClass>
void tzbx15_device::roundupt_drawgfxzoomrotate(
		BitmapClass &dest_bmp, const rectangle &clip,
		gfx_element *gfx, uint32_t code, uint32_t color,
		int flipx, int flipy, uint32_t ssx, uint32_t ssy,
		int scalex, int scaley, int rotate,
		int write_priority_only)
{
	if (!scalex || !scaley) return;

	/*
	scalex and scaley are 16.16 fixed point numbers
	1<<15 : shrink to 50%
	1<<16 : uniform scale
	1<<17 : double to 200%
	*/

	// force clip to bitmap boundary
	rectangle myclip = clip;
	myclip &= dest_bmp.cliprect();

	if (gfx)
	{
		const pen_t *pal = &m_palette_clut->pen(gfx->colorbase() + gfx->granularity() * (color % gfx->colors()));
		const uint8_t *shadow_pens = m_shadow_pen_array.get() + (gfx->granularity() * (color % gfx->colors()));
		const uint8_t *code_base = gfx->get_data(code % gfx->elements());
		const int clut_offset = gfx->granularity() * (color % gfx->colors());

		int block_size = 8 * scalex;
		int sprite_screen_height = ((ssy&0xffff)+block_size)>>16;
		int sprite_screen_width = ((ssx&0xffff)+block_size)>>16;

		if (sprite_screen_width && sprite_screen_height)
		{
			// compute sprite increment per screen pixel
			int dx = (gfx->width()<<16)/sprite_screen_width;
			int dy = (gfx->height()<<16)/sprite_screen_height;

			int sx;//=ssx>>16;
			int sy;//=ssy>>16;

//          int ex = sx+sprite_screen_width;
//          int ey = sy+sprite_screen_height;

			int incxx=0x10000;//(int)((float)dx * cos(theta));
//          int incxy=0x0;//(int)((float)dy * -sin(theta));
			int incyx=0x0;//(int)((float)dx * sin(theta));
//          int incyy=0x10000;//(int)((float)dy * cos(theta));

			if (ssx&0x80000000) sx=0-(0x10000 - (ssx>>16)); else sx=ssx>>16;
			if (ssy&0x80000000) sy=0-(0x10000 - (ssy>>16)); else sy=ssy>>16;
			int ex = sx+sprite_screen_width;
			int ey = sy+sprite_screen_height;
			int x_index_base;
			if( flipx )
			{
				x_index_base = (sprite_screen_width-1)*dx;
				dx = -dx;
				incxx=-incxx;
				incyx=-incyx;
			}
			else
			{
				x_index_base = 0;
			}

			int y_index;
			if( flipy )
			{
				y_index = (sprite_screen_height-1)*dy;
				dy = -dy;
			}
			else
			{
				y_index = 0;
			}

			if( sx < myclip.left())
			{
				// clip left
				int pixels = myclip.left()-sx;
				sx += pixels;
				x_index_base += pixels*dx;
			}
			if( sy < myclip.top() )
			{
				// clip top
				int pixels = myclip.top()-sy;
				sy += pixels;
				y_index += pixels*dy;
			}
			if( ex > myclip.right()+1 )
			{
				// clip right
				int pixels = ex-myclip.right()-1;
				ex -= pixels;
			}
			if( ey > myclip.bottom()+1 )
			{
				// clip bottom
				int pixels = ey-myclip.bottom()-1;
				ey -= pixels;
			}

			// skip if inner loop doesn't draw anything
			if( ex > sx )
			{
				if constexpr (std::is_same_v<BitmapClass, bitmap_ind16>)
					m_rot_bounds |= rectangle(sx, ex - 1, sy, ey - 1);

#if 0
				int startx=0;
				int starty=0;

				//int incxx=0x10000;
				//int incxy=0;
				//int incyx=0;
				//int incyy=0x10000;
				double theta=rotate * ((2.0 * std::numbers::pi)/512.0);
				double c=cos(theta);
				double s=sin(theta);

				//if (ey-sy > 0) dy=dy / (ey-sy);
				{
					float angleAsRadians=(float)rotate * (7.28f / 512.0f);
					//float ccx = cosf(angleAsRadians);
					//float ccy = sinf(angleAsRadians);
					float a=0;

				}

				for( int y=sy; y<ey; y++ )
				{
					uint32_t *const dest = &dest_bmp.pix(y);
					int cx = startx;
					int cy = starty;

					int x_index = x_index_base;
					for( int x=sx; x<ex; x++ )
					{
						const uint8_t *source = code_base + (cy>>16) * gfx->rowbytes();
						int c = source[(cx >> 16)];
						if( c != transparent_color )
						{
							if (write_priority_only)
								dest[x]=shadow_pens[c];
							else
								dest[x]=pal[c];
						}
						cx += incxx;
						cy += incxy;
					}
					startx += incyx;
					starty += incyy;
				}
#endif
#if 1 // old
				for( int y=sy; y<ey; y++ )
				{
					uint8_t const *const source = code_base + (y_index>>16) * gfx->rowbytes();
					typename BitmapClass::pixel_t *const dest = &dest_bmp.pix(y);

					int x_index = x_index_base;
					for( int x=sx; x<ex; x++ )
					{
						int c = source[x_index>>16];
						if( c )
						{
							// Rotation scratch buffer: keep the CLUT index, resolved when the sprite is rotated onto the screen
							if constexpr (std::is_same_v<BitmapClass, bitmap_ind16>)
								dest[x]=clut_offset + c + 1;
							// Only draw shadow pens if writing priority buffer
							else if (write_priority_only)
								dest[x]=shadow_pens[c];
							else if (!shadow_pens[c])
								dest[x]=pal[c];
						}
						x_index += dx;
					}

					y_index += dy;
				}
#endif
			}
		}
	}
}

/*
    Sprite RAM itself uses an index into two ROM tables to actually draw the object.

    Sprite RAM format:

    Word 0: 0xf000 - ?
            0x0fff - Index into ROM sprite table
    Word 1: 0x8000 - X Flip
            0x4000 - Y Flip
            0x3000 - ?
            0x0ff8 - Color
            0x0007 - ?
    Word 2: 0xffff - X position
    Word 3: 0xffff - Y position
    Word 4: 0x01ff - Scale
    Word 5: 0x01ff - Rotation

    Sprite ROM table format, alternate lines come from each bank, with the
    very first line indicating control information:

    First bank:
    Byte 0: Y destination offset (in scanlines, unaffected by scale).
    Byte 1: Always 0?
    Byte 2: Number of source scanlines to render from (so unaffected by destination scale).
    Byte 3: Usually 0, sometimes 0x80??

    Other banks:
    Byte 0: Width of line in tiles (-1)
    Byte 1: X offset to start drawing line at (multipled by scale * 8)
    Bytes 2/3: Tile index to start fetching tiles from (increments per tile).

*/
template<class BitmapClass>
void tzbx15_device::draw_sprites_main(BitmapClass &bitmap, const rectangle &cliprect, int write_priority_only, int rambank)
{
	// Sprite data is double buffered
	for (int offs = rambank;offs < rambank + 0x800;offs += 6)
	{
		int y =         m_spriteram[offs+3];
		int x =         m_spriteram[offs+2];
		int scale =     m_spriteram[offs+4] & 0x1ff;
		int color =     m_spriteram[offs+1] >> 3 & 0x1ff;
		int flip_x =    m_spriteram[offs+1] & 0x8000;
		int flip_y =    m_spriteram[offs+1] & 0x4000;
		int rotate =    m_spriteram[offs+5] & 0x1ff;
		bool const rotated = m_rotation_enabled && rotate;

		int index = m_spriteram[offs];

//      if (m_spriteram[offs+1]&0x7)
//          color=machine().rand()%0xff;

		/* End of sprite list marker */
		if (index == 0xffff || m_spriteram[offs + 4] == 0xffff) // todo
			return;

		if (index >= 0x4000)
			continue;

		uint8_t const *src1 = m_sprites_l_rom + (index * 4);
		uint8_t const *src2 = m_sprites_h_rom + (index * 4);

		int lines = src1[2];
		int y_offset = src1[0]&0xf8;
		// Header bit 0x80 repeats each line's tiles on towards the screen edge (Apache 3: the
		// splash backdrop strip, mountain ranges, bush and tree rows that cover the whole width)
		bool const repeat = m_line_repeat_enabled && (src1[3] & 0x80);

		lines -= y_offset;

		// A rotated sprite is first drawn unrotated into a scratch buffer with its
		// origin at ROT_ORIGIN, then rotated onto the screen by draw_rotated()
		int render_x = (rotated ? ROT_ORIGIN : x) << 16;
		int render_y = (rotated ? ROT_ORIGIN : y) << 16;
		scale = scale << 9; /* 0x80 becomes 0x10000 */

		if (flip_y)
			render_y -= y_offset * scale;
		else
			render_y += y_offset * scale;

		// A Y-flipped sprite covers the mirror image of its box, ending at its origin like an
		// X-flipped one; MAME has always drawn it one line lower. Apache 3's object tables put
		// the attract rotor's hub at +/-112.9 in both cases, and the X+Y flipped exposure
		// visibly dropped below the mast without this.
		if (flip_y && m_rotation_enabled)
			render_y -= scale * 8;

		src1 += 4;
		int h = 0;

		while (lines > 0)
		{
			int base, x_offs, x_width, x_pos;

			/* Odd and even lines come from different banks */
			if (h & 1)
			{
				x_width = src1[0] + 1;
				x_offs = src1[1] * scale * 8;
				base = src1[2] | (src1[3] << 8);
			}
			else
			{
				x_width = src2[0] + 1;
				x_offs = src2[1] * scale * 8;
				base = src2[2] | (src2[3] << 8);
			}

			base *= 2;

			if (flip_x)
				x_pos = render_x - x_offs - scale * 8;
			else
				x_pos = render_x + x_offs;

			rectangle const &region = rotated ? m_rot_bitmap.cliprect() : cliprect;
			int const base0 = base;
			for (int w = 0; w < (repeat ? MAX_REPEAT_TILES : x_width); w++)
			{
				if (w >= x_width)
				{
					if (flip_x ? (((x_pos + scale * 8) >> 16) < region.left()) : ((x_pos >> 16) > region.right()))
						break;
					base = base0 + (w % x_width);
				}
				if (rotated)
					roundupt_drawgfxzoomrotate(
							m_rot_bitmap,m_rot_bitmap.cliprect(),gfx(0 + (base & 1)),
							base >> 1,
							color,flip_x,flip_y,x_pos,render_y,
							scale,scale,0,write_priority_only);
				else
					roundupt_drawgfxzoomrotate(
							bitmap,cliprect,gfx(0 + (base & 1)),
							base >> 1,
							color,flip_x,flip_y,x_pos,render_y,
							scale,scale,0,write_priority_only);
				base++;

				if (flip_x)
					x_pos -= scale * 8;
				else
					x_pos += scale * 8;
			}
			if (h & 1)
				src1 += 4;
			else
				src2 += 4;

			if (flip_y)
				render_y -= 8 * scale;
			else
				render_y += 8 * scale;

			h++;
			lines -= 8;
		}

		if (rotated)
			draw_rotated(bitmap, cliprect, int16_t(x), int16_t(y), rotate, flip_x, write_priority_only);
	}
}

/*
    Copy the scratch buffer onto the screen, rotated by 'rotate' (0x200 = full turn)
    about the sprite origin, which lands on screen position (x, y).
*/
template<class BitmapClass>
void tzbx15_device::draw_rotated(BitmapClass &bitmap, const rectangle &cliprect, int x, int y, int rotate, bool flip_x, int write_priority_only)
{
	// bounds of what was drawn into the scratch buffer
	rectangle const src = m_rot_bounds;
	m_rot_bounds = EMPTY_BOUNDS;
	if (src.empty())
		return;

	/*
	    The rotation value is a signed 9-bit slope t (0x100 = 1.0) and the chip maps the drawn
	    (flipped) sprite through [1 -t; t 1] about its origin, which turns it by atan(t) and
	    also enlarges it by sqrt(1 + t^2); flip X turns it the other way. Games shrink the scale
	    to compensate. Derived from Apache 3:
	    - ground objects carry t = tan(bank angle) (0xa9 at a 33 degree bank) and their scale
	      shrinks by 1/sqrt(1 + t^2), and the mountain strip's origin stays at a fixed distance
	      from the horizon, so the base of the strip stays on the ground plane;
	    - its 68000 object tables (0x92e4) offset positions by exactly M * (sprite centre), for
	      the attract rotor and the title logo, unflipped and X+Y flipped;
	    - mirrored ground objects (flip X, -t) lean with the bank like their unmirrored twins,
	      and the logo turns on continuously through its X+Y flipped phases.
	*/
	double const t = util::sext(rotate, 9) / 256.0 * (flip_x ? -1.0 : 1.0);
	auto const forward = [t] (double lx, double ly) { return std::make_pair(lx - t * ly, t * lx + ly); };

	// screen bounding box of the transformed scratch rectangle
	double minx = 1e9, maxx = -1e9, miny = 1e9, maxy = -1e9;
	for (int corner = 0; corner < 4; corner++)
	{
		auto const [dx, dy] = forward(
				((corner & 1) ? src.right() + 1 : src.left()) - ROT_ORIGIN,
				((corner & 2) ? src.bottom() + 1 : src.top()) - ROT_ORIGIN);
		minx = std::min(minx, dx); maxx = std::max(maxx, dx);
		miny = std::min(miny, dy); maxy = std::max(maxy, dy);
	}
	rectangle dst(x + int(floor(minx)) - 1, x + int(ceil(maxx)) + 1, y + int(floor(miny)) - 1, y + int(ceil(maxy)) + 1);
	dst &= cliprect;
	dst &= bitmap.cliprect();

	// inverse: M^-1 = [1 t; -t 1] / (1 + t^2)
	double const k = 1.0 / (1.0 + t * t);
	for (int py = dst.top(); py <= dst.bottom(); py++)
	{
		typename BitmapClass::pixel_t *const dest = &bitmap.pix(py);
		double const dy = py + 0.5 - y;
		for (int px = dst.left(); px <= dst.right(); px++)
		{
			double const dx = px + 0.5 - x;
			int const sx = int(floor((dx + t * dy) * k)) + ROT_ORIGIN;
			int const sy = int(floor((dy - t * dx) * k)) + ROT_ORIGIN;
			if (!src.contains(sx, sy))
				continue;
			uint16_t const v = m_rot_bitmap.pix(sy, sx);
			if (!v)
				continue;
			int const pen = v - 1;
			if (write_priority_only)
				dest[px] = m_shadow_pen_array[pen];
			else if (!m_shadow_pen_array[pen])
				dest[px] = m_palette_clut->pen(pen);
		}
	}

	m_rot_bitmap.fill(0, src);
}


void tzbx15_device::draw_sprites(bitmap_rgb32 &bitmap, const rectangle &cliprect, int write_priority_only, int rambank)
{
	draw_sprites_main(bitmap, cliprect, write_priority_only, rambank);
}

void tzbx15_device::draw_sprites(bitmap_ind8& bitmap, const rectangle &cliprect, int write_priority_only, int rambank)
{
	draw_sprites_main(bitmap, cliprect, write_priority_only, rambank);
}

/*
 * Object palettes are build from a series of cluts stored in the object roms.
 *
 *  We update 'Mame palettes' from the clut here in order to simplify the
 *  draw routines.  We also note down any uses of the 'shadow' pen (index 255).
 */
void tzbx15_device::update_cluts()
{
	const int length = m_rom_clut_size * 2;
	const uint8_t* bank1 = m_sprites_l_rom + m_rom_clut_offset;
	const uint8_t* bank2 = m_sprites_h_rom + m_rom_clut_offset;

	for (int i = 0; i < length; i+=8)
	{
		m_palette_clut->set_pen_color(i + 0, m_palette_base->pen_color(bank1[1] + m_sprite_palette_base));
		m_shadow_pen_array[i+0]=(bank1[1] == 255);
		m_palette_clut->set_pen_color(i + 1, m_palette_base->pen_color(bank1[0] + m_sprite_palette_base));
		m_shadow_pen_array[i+1]=(bank1[0] == 255);
		m_palette_clut->set_pen_color(i + 2, m_palette_base->pen_color(bank1[3] + m_sprite_palette_base));
		m_shadow_pen_array[i+2]=(bank1[3] == 255);
		m_palette_clut->set_pen_color(i + 3, m_palette_base->pen_color(bank1[2] + m_sprite_palette_base));
		m_shadow_pen_array[i+3]=(bank1[2] == 255);

		m_palette_clut->set_pen_color(i + 4, m_palette_base->pen_color(bank2[1] + m_sprite_palette_base));
		m_shadow_pen_array[i+4]=(bank2[1] == 255);
		m_palette_clut->set_pen_color(i + 5, m_palette_base->pen_color(bank2[0] + m_sprite_palette_base));
		m_shadow_pen_array[i+5]=(bank2[0] == 255);
		m_palette_clut->set_pen_color(i + 6, m_palette_base->pen_color(bank2[3] + m_sprite_palette_base));
		m_shadow_pen_array[i+6]=(bank2[3] == 255);
		m_palette_clut->set_pen_color(i + 7, m_palette_base->pen_color(bank2[2] + m_sprite_palette_base));
		m_shadow_pen_array[i+7]=(bank2[2] == 255);

		bank1+=4;
		bank2+=4;
	}
}
