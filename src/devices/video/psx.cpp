// license:BSD-3-Clause
// copyright-holders:smf
/*
 * PlayStation GPU emulator
 *
 * Copyright 2003-2014 smf
 *
 */

#include "emu.h"
#include "psx.h"

#include "cpu/psx/psx.h"

#include "osdepend.h"
#include "interface/gpurender.h"

#include "screen.h"

#include <algorithm>
#include <cmath>


#define STOP_ON_ERROR ( 0 )

#define LOG_WRITE        (1U << 1)
#define LOG_READ         (1U << 2)
#define LOG_TRANSPARENCY (1U << 3)
#define VERBOSE          (0)
#include "logmacro.h"

// device type definition
DEFINE_DEVICE_TYPE(CXD8514Q,  cxd8514q_device,  "cxd8514q",  "CXD8514Q GPU") // VRAM
DEFINE_DEVICE_TYPE(CXD8538Q,  cxd8538q_device,  "cxd8538q",  "CXD8538Q GPU") // VRAM
DEFINE_DEVICE_TYPE(CXD8561Q,  cxd8561q_device,  "cxd8561q",  "CXD8561Q GPU") // SGRAM
DEFINE_DEVICE_TYPE(CXD8561BQ, cxd8561bq_device, "cxd8561bq", "CXD8561BQ GPU") // SGRAM
DEFINE_DEVICE_TYPE(CXD8561CQ, cxd8561cq_device, "cxd8561cq", "CXD8561CQ GPU") // SGRAM
DEFINE_DEVICE_TYPE(CXD8654Q,  cxd8654q_device,  "cxd8654q",  "CXD8654Q GPU") // SGRAM

psxgpu_device::psxgpu_device(const machine_config &mconfig, device_type type, const char *tag, device_t *owner, uint32_t clock)
	: device_t(mconfig, type, tag, owner, clock)
	, device_video_interface(mconfig, *this)
	, device_palette_interface(mconfig, *this)
	, m_ram(*this, finder_base::DUMMY_TAG)
	, m_vblank_handler(*this)
	, m_vclk{ 0, 0 }
{
}

void psxgpu_device::set_cpu(psxcpu_device* cpu)
{
	cpu->gpu_read().set(*this, FUNC(psxgpu_device::read));
	cpu->gpu_write().set(*this, FUNC(psxgpu_device::write));
	cpu->subdevice<psxdma_device>("dma")->install_read_handler(2, psxdma_device::read_delegate(&psxgpu_device::dma_read, this));
	cpu->subdevice<psxdma_device>("dma")->install_write_handler(2, psxdma_device::write_delegate(&psxgpu_device::dma_write, this));
	vblank_callback().set(*cpu->subdevice<psxirq_device>("irq"), FUNC(psxirq_device::intin0));
}

void psxgpu_device::device_start()
{
	screen().register_vblank_callback(vblank_state_delegate(&psxgpu_device::vblank, this));

	for( int n_colour = 0; n_colour < 0x10000; n_colour++ )
	{
		set_pen_color( n_colour, pal555(n_colour,0, 5, 10) );
	}

	// GPU-accelerated rendering (Phase 2, see CLAUDE.md): deliberately NOT
	// acquired here. device_start()/device_reset() run synchronously
	// inside retro_load_game(), before RetroArch has created its own
	// Wayland/EGL context - creating ours this early was found to corrupt
	// RetroArch's later context setup (EGL_BAD_CONTEXT, then a crash),
	// regardless of EGL platform choice or careful current-context
	// save/restore. gpu_active() acquires it lazily on first real use
	// instead, which only happens once actual gameplay/rendering is
	// underway (well after RetroArch's own setup completes).

	if (type() == CXD8538Q)
	{
		psx_gpu_init( 1 );
	}
	else
	{
		psx_gpu_init( 2 );
	}

	// mame_psx_gpu_pgxp core option (CLAUDE.md Phase 4) - read once at
	// boot, same as gpu_scale()/mame_psx_gpu_hle. Only meaningful when the
	// GPU HLE path itself is active; harmless (never consulted) otherwise.
	m_gpu_pgxp_enabled = machine().osd().gpu_render_pgxp_enabled();
	m_gpu_pgxp_tolerance = machine().osd().gpu_render_pgxp_tolerance();
	m_gpu_pgxp_vcache = m_gpu_pgxp_enabled && machine().osd().gpu_render_pgxp_vertex_cache();
	if( m_cpu != nullptr )
	{
		m_cpu->set_pgxp_vertex_cache_enabled( m_gpu_pgxp_vcache );
	}
	if( m_cpu != nullptr )
	{
		m_cpu->set_pgxp_enabled( m_gpu_pgxp_enabled );
	}
}

void psxgpu_device::device_reset()
{
	gpu_reset();
}

int psxgpu_device::gpu_scale() const
{
	return machine().osd().gpu_render_scale();
}

bool psxgpu_device::gpu_active()
{
	// Lazily acquired here, on first real use (a polygon submission or
	// update_screen(), both of which only happen once actual gameplay
	// execution is underway) rather than in device_start()/device_reset()
	// - see the comment there for why.
	if (!m_gpu_active_checked)
	{
		m_gpu_active_checked = true;
		m_gpu_render_target = machine().osd().get_gpu_render_target();
		// No context/frame priming here - all GPU work happens in one
		// batched burst per frame in gpu_update_screen() (see the
		// m_gpu_queue comment in psx.h), not incrementally as primitives
		// are submitted.
	}
	return m_gpu_render_target != nullptr;
}

cxd8514q_device::cxd8514q_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock)
	: psxgpu_device(mconfig, CXD8514Q, tag, owner, clock)
{
}

cxd8538q_device::cxd8538q_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock)
	: psxgpu_device(mconfig, CXD8538Q, tag, owner, clock)
{
}

cxd8561q_device::cxd8561q_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock)
	: psxgpu_device(mconfig, CXD8561Q, tag, owner, clock)
{
}

cxd8561bq_device::cxd8561bq_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock)
	: psxgpu_device(mconfig, CXD8561BQ, tag, owner, clock)
{
}

cxd8561cq_device::cxd8561cq_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock)
	: psxgpu_device(mconfig, CXD8561CQ, tag, owner, clock)
{
}

cxd8654q_device::cxd8654q_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock)
	: psxgpu_device(mconfig, CXD8654Q, tag, owner, clock)
{
}

static const int m_p_n_nextpointlist4[] = { 1, 3, 0, 2 };
static const int m_p_n_prevpointlist4[] = { 2, 0, 3, 1 };
static const int m_p_n_nextpointlist4b[] = { 0, 3, 1, 2 };
static const int m_p_n_prevpointlist4b[] = { 0, 2, 3, 1 };
static const int m_p_n_nextpointlist3[] = { 1, 2, 0 };
static const int m_p_n_prevpointlist3[] = { 2, 0, 1 };

#define COORD_X( a ) ( a.sw.l )
#define COORD_Y( a ) ( a.sw.h )
#define S11_COORD_X( a ) util::sext( a.sw.l, 11 )
#define S11_COORD_Y( a ) util::sext( a.sw.h, 11 )
#define SIZE_W( a ) ( a.w.l )
#define SIZE_H( a ) ( a.w.h )
#define BGR_C( a ) ( a.b.h3 )
#define BGR_B( a ) ( a.b.h2 )
#define BGR_G( a ) ( a.b.h )
#define BGR_R( a ) ( a.b.l )
#define TEXTURE_V( a ) ( a.b.h )
#define TEXTURE_U( a ) ( a.b.l )

#if PSXGPU_DEBUG_VIEWER

void psxgpu_device::DebugMeshInit()
{
	int width = screen().width();
	int height = screen().height();

	m_debug.b_mesh = 0;
	m_debug.b_texture = 0;
	m_debug.n_interleave = -1;
	m_debug.b_clear = 1;
	m_debug.n_coord = 0;
	m_debug.n_skip = 0;
	m_debug.mesh = std::make_unique<bitmap_ind16>(width, height );
}

void psxgpu_device::DebugMesh( int n_coordx, int n_coordy )
{
	int width = screen().width();
	int height = screen().height();

	n_coordx += m_n_displaystartx;
	n_coordy += n_displaystarty;

	if( m_debug.b_clear )
	{
		m_debug.mesh->fill(0x0000);
		m_debug.b_clear = 0;
	}

	int n_coord;
	int n_colour = 0x1f;

	for( n_coord = 0; n_coord < m_debug.n_coord; n_coord++ )
	{
		if( n_coordx != m_debug.n_coordx[ n_coord ] ||
			n_coordy != m_debug.n_coordy[ n_coord ] )
		{
			break;
		}
	}

	if( n_coord == m_debug.n_coord && m_debug.n_coord > 1 )
	{
		n_colour = 0xffff;
	}

	for( n_coord = 0; n_coord < m_debug.n_coord; n_coord++ )
	{
		int32_t n_xstart = m_debug.n_coordx[ n_coord ];
		int32_t n_xend = n_coordx;
		int32_t n_xlen;
		if( n_xend > n_xstart )
		{
			n_xlen = n_xend - n_xstart;
		}
		else
		{
			n_xlen = n_xstart - n_xend;
		}

		int32_t n_ystart = m_debug.n_coordy[ n_coord ];
		int32_t n_yend = n_coordy;
		int32_t n_ylen;
		if( n_yend > n_ystart )
		{
			n_ylen = n_yend - n_ystart;
		}
		else
		{
			n_ylen = n_ystart - n_yend;
		}

		int32_t n_len;
		if( n_xlen > n_ylen )
		{
			n_len = n_xlen;
		}
		else
		{
			n_len = n_ylen;
		}

		PAIR n_x; n_x.sw.h = n_xstart; n_x.sw.l = 0;
		PAIR n_y; n_y.sw.h = n_ystart; n_y.sw.l = 0;

		if( n_len == 0 )
		{
			n_len = 1;
		}

		int32_t n_dx = (int32_t)( ( n_xend << 16 ) - n_x.d ) / n_len;
		int32_t n_dy = (int32_t)( ( n_yend << 16 ) - n_y.d ) / n_len;

		while( n_len > 0 )
		{
			if( (int16_t)n_x.w.h >= 0 &&
				(int16_t)n_y.w.h >= 0 &&
				(int16_t)n_x.w.h <= width - 1 &&
				(int16_t)n_y.w.h <= height - 1 )
			{
				if( m_debug.mesh->pix( n_y.w.h, n_x.w.h ) != 0xffff )
					m_debug.mesh->pix( n_y.w.h, n_x.w.h ) = n_colour;
			}

			n_x.d += n_dx;
			n_y.d += n_dy;
			n_len--;
		}
	}

	if( m_debug.n_coord < DEBUG_COORDS )
	{
		m_debug.n_coordx[ m_debug.n_coord ] = n_coordx;
		m_debug.n_coordy[ m_debug.n_coord ] = n_coordy;
		m_debug.n_coord++;
	}
}

void psxgpu_device::DebugMeshEnd()
{
	m_debug.n_coord = 0;
}

void psxgpu_device::DebugCheckKeys()
{
	if( machine().input().code_pressed_once( KEYCODE_M ) )
	{
		m_debug.b_mesh = !m_debug.b_mesh;
		updatevisiblearea();
	}

	if( machine().input().code_pressed_once( KEYCODE_V ) )
	{
		m_debug.b_texture = !m_debug.b_texture;
		updatevisiblearea();
	}

	if( machine().input().code_pressed_once( KEYCODE_I ) )
	{
		if( m_debug.b_texture )
		{
			m_debug.n_interleave++;

			if( m_debug.n_interleave == 2 )
				m_debug.n_interleave = -1;

			if( m_debug.n_interleave == -1 )
				popmessage( "interleave off" );
			else if( m_debug.n_interleave == 0 )
				popmessage( "4 bit interleave" );
			else if( m_debug.n_interleave == 1 )
				popmessage( "8 bit interleave" );
		}
		else
		{
			m_debug.n_skip++;

			if( m_debug.n_skip > 15 )
				m_debug.n_skip = 0;

			popmessage( "debug skip %d", m_debug.n_skip );
		}
	}

#if 0
	if( machine().input().code_pressed_once( KEYCODE_D ) )
	{
		FILE *f = fopen( "dump.txt", "w" );
		for( int n_y = 256; n_y < 512; n_y++ )
			for( int n_x = 640; n_x < 1024; n_x++ )
				fprintf( f, "%04u,%04u = %04x\n", n_y, n_x, p_p_vram[ n_y ][ n_x ] );
		fclose( f );
	}
	if( machine().input().code_pressed_once( KEYCODE_S ) )
	{
		popmessage( "saving..." );

		FILE *f = fopen( "VRAM.BIN", "wb" );
		for( int n_y = 0; n_y < 1024; n_y++ )
			fwrite( p_p_vram[ n_y ], 1024 * 2, 1, f );
		fclose( f );
	}
	if( machine().input().code_pressed_once( KEYCODE_L ) )
	{
		popmessage( "loading..." );

		FILE *f = fopen( "VRAM.BIN", "rb" );
		for( int n_y = 0; n_y < 1024; n_y++ )
			fread( p_p_vram[ n_y ], 1024 * 2, 1, f );
		fclose( f );
	}
#endif
}

int psxgpu_device::DebugMeshDisplay( bitmap_rgb32 &bitmap, const rectangle &cliprect )
{
	if( m_debug.b_mesh )
	{
		for( int y = cliprect.min_y; y <= cliprect.max_y; y++ )
			draw_scanline16( bitmap, cliprect.min_x, y, cliprect.max_x + 1 - cliprect.min_x, &m_debug.mesh->pix( y ), pens() );
	}

	m_debug.b_clear = 1;
	return m_debug.b_mesh;
}

int psxgpu_device::DebugTextureDisplay( bitmap_rgb32 &bitmap )
{
	if( m_debug.b_texture )
	{
		int width = screen().width();
		int height = screen().height();

		for( int n_y = 0; n_y < height; n_y++ )
		{
			uint16_t p_n_interleave[ 1024 ];

			for( int n_x = 0; n_x < width; n_x++ )
			{
				int n_xi;
				int n_yi;

				if( m_debug.n_interleave == 0 )
				{
					n_xi = ( n_x & ~0x3c ) + ( ( n_y << 2 ) & 0x3c );
					n_yi = ( n_y & ~0xf ) + ( ( n_x >> 2 ) & 0xf );
				}
				else if( m_debug.n_interleave == 1 )
				{
					n_xi = ( n_x & ~0x78 ) + ( ( n_x << 3 ) & 0x40 ) + ( ( n_y << 3 ) & 0x38 );
					n_yi = ( n_y & ~0x7 ) + ( ( n_x >> 4 ) & 0x7 );
				}
				else
				{
					n_xi = n_x;
					n_yi = n_y;
				}

				p_n_interleave[ n_x ] = p_p_vram[ n_yi ][ n_xi ];
			}

			draw_scanline16( bitmap, 0, n_y, width, p_n_interleave, pens() );
		}
	}

	return m_debug.b_texture;
}

#endif

void psxgpu_device::updatevisiblearea()
{
	rectangle visarea;
	double refresh;

	if( ( n_gpustatus & ( 1 << 0x14 ) ) != 0 )
	{
		/* pal */
		refresh = 50; // TODO: it's not exactly 50Hz
		switch( ( n_gpustatus >> 0x13 ) & 1 )
		{
		case 0:
			n_screenheight = 256;
			break;
		case 1:
			n_screenheight = 512;
			break;
		}
	}
	else
	{
		/* ntsc */
		// refresh rate derived from 53.693175MHz
		// TODO: emulate display timings at lower level
		switch( ( n_gpustatus >> 0x13 ) & 1 )
		{
		case 0:
			refresh = 59.8260978565;
			n_screenheight = 240;
			break;
		case 1:
			refresh = 59.9400523286;
			n_screenheight = 480;
			break;
		}
	}
	switch( ( n_gpustatus >> 0x11 ) & 3 )
	{
	case 0:
		switch( ( n_gpustatus >> 0x10 ) & 1 )
		{
		case 0:
			n_screenwidth = 256;
			break;
		case 1:
			n_screenwidth = 368;
			break;
		}
		break;
	case 1:
		switch( ( n_gpustatus >> 0x10 ) & 1 )
		{
		case 0:
			n_screenwidth = 320;
			break;
		case 1:
			n_screenwidth = 384;
			break;
		}
		break;
	case 2:
		n_screenwidth = 512;
		break;
	case 3:
		n_screenwidth = 640;
		break;
	}

#if PSXGPU_DEBUG_VIEWER
	if( m_debug.b_mesh || m_debug.b_texture )
	{
		n_screenheight = 1024;
		n_screenwidth = 1024;
	}
#endif

	// Cheap compile-time-only query (see osdepend.h) - deliberately NOT
	// gpu_active(), which lazily constructs a real GPU context and must
	// stay deferred until actual gameplay execution starts (see
	// device_start() and gpu_active() for why). This just decides whether
	// to declare a scaled screen size; the real context/begin_frame() get
	// created lazily, on first actual use, in gpu_active() itself.
	if (machine().osd().gpu_render_available())
	{
		// n_screenwidth/n_screenheight stay native - they're used
		// throughout for VRAM/draw-area addressing - only the declared
		// screen size (what MAME/the OSD report as this screen's
		// resolution) is scaled up.
		uint32_t scaled_width = n_screenwidth * gpu_scale();
		uint32_t scaled_height = n_screenheight * gpu_scale();
		visarea.set(0, scaled_width - 1, 0, scaled_height - 1);

		// screen.configure() can synchronously propagate to the libretro
		// OSD's RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO callback when the size
		// actually changes, and RetroArch's frontend reacts to that with its
		// own EGL work right there, on this same thread - a real problem
		// when our GPU context used to be held bound across multiple calls,
		// but this runs before gpu_update_screen()'s batched GPU work for
		// the frame even starts (see the m_gpu_queue comment in psx.h), so
		// our context is never bound at this point - nothing to guard here.
		screen().configure(scaled_width, scaled_height, visarea, attotime::from_hz(refresh));
	}
	else
	{
		visarea.set(0, n_screenwidth - 1, 0, n_screenheight - 1);
		screen().configure(n_screenwidth, n_screenheight, visarea, attotime::from_hz(refresh));
	}
}

void psxgpu_device::psx_gpu_init( int n_gputype )
{
	int width = 1024;
	int height = ( m_ram->size() / width ) / sizeof( uint16_t );

	m_n_gputype = n_gputype;

#if PSXGPU_DEBUG_VIEWER
	DebugMeshInit();
#endif

	n_gpustatus = 0x14802000;
	n_gpuinfo = 0;
	n_gpu_buffer_offset = 0;
	n_lightgun_x = 0;
	n_lightgun_y = 0;
	b_reverseflag = 0;

	p_vram = m_ram->pointer<uint16_t>();
	m_vram_height = height;

	for( int n_line = 0; n_line < 1024; n_line++ )
	{
		p_p_vram[ n_line ] = &p_vram[ ( n_line % height ) * width ];
	}

	for( int n_level = 0; n_level < MAX_LEVEL; n_level++ )
	{
		for( int n_shade = 0; n_shade < MAX_SHADE; n_shade++ )
		{
			/* shaded */
			int n_shaded = ( n_level * n_shade ) / MID_SHADE;
			if( n_shaded > MAX_LEVEL - 1 )
			{
				n_shaded = MAX_LEVEL - 1;
			}
			p_n_redshade[ ( n_level * MAX_SHADE ) | n_shade ] = n_shaded;
			p_n_greenshade[ ( n_level * MAX_SHADE ) | n_shade ] = n_shaded << 5;
			p_n_blueshade[ ( n_level * MAX_SHADE ) | n_shade ] = n_shaded << 10;

			/* 1/4 x transparency */
			n_shaded = ( n_level * n_shade ) / MID_SHADE;
			n_shaded >>= 2;
			if( n_shaded > MAX_LEVEL - 1 )
			{
				n_shaded = MAX_LEVEL - 1;
			}
			p_n_f025[ ( n_level * MAX_SHADE ) | n_shade ] = n_shaded;

			/* 1/2 x transparency */
			n_shaded = ( n_level * n_shade ) / MID_SHADE;
			n_shaded >>= 1;
			if( n_shaded > MAX_LEVEL - 1 )
			{
				n_shaded = MAX_LEVEL - 1;
			}
			p_n_f05[ ( n_level * MAX_SHADE ) | n_shade ] = n_shaded;

			/* 1 x transparency */
			n_shaded = ( n_level * n_shade ) / MID_SHADE;
			if( n_shaded > MAX_LEVEL - 1 )
			{
				n_shaded = MAX_LEVEL - 1;
			}
			p_n_f1[ ( n_level * MAX_SHADE ) | n_shade ] = n_shaded;
		}
	}

	for( int n_level = 0; n_level < 0x10000; n_level++ )
	{
		p_n_redlevel[ n_level ] = ( n_level & ( MAX_LEVEL - 1 ) ) * MAX_SHADE;
		p_n_greenlevel[ n_level ] = ( ( n_level >> 5 ) & ( MAX_LEVEL - 1 ) ) * MAX_SHADE;
		p_n_bluelevel[ n_level ] = ( ( n_level >> 10 ) & ( MAX_LEVEL - 1 ) ) * MAX_SHADE;

		/* 0.5 * background */
		p_n_redb05[ n_level ] = ( ( n_level & ( MAX_LEVEL - 1 ) ) / 2 ) * MAX_LEVEL;
		p_n_greenb05[ n_level ] = ( ( ( n_level >> 5 ) & ( MAX_LEVEL - 1 ) ) / 2 ) * MAX_LEVEL;
		p_n_blueb05[ n_level ] = ( ( ( n_level >> 10 ) & ( MAX_LEVEL - 1 ) ) / 2 ) * MAX_LEVEL;

		/* 1 * background */
		p_n_redb1[ n_level ] = ( n_level & ( MAX_LEVEL - 1 ) ) * MAX_LEVEL;
		p_n_greenb1[ n_level ] = ( ( n_level >> 5 ) & ( MAX_LEVEL - 1 ) ) * MAX_LEVEL;
		p_n_blueb1[ n_level ] = ( ( n_level >> 10 ) & ( MAX_LEVEL - 1 ) ) * MAX_LEVEL;

		/* 24bit color */
		p_n_g0r0[ n_level ] = ( ( ( n_level >> 8 ) & 0xff ) << 8 ) | ( ( ( n_level >> 0 ) & 0xff ) << 16 );
		p_n_b0[ n_level ] = ( ( n_level >> 0 ) & 0xff ) << 0;
		p_n_r1[ n_level ] = ( ( n_level >> 8 ) & 0xff ) << 16;
		p_n_b1g1[ n_level ] = ( ( ( n_level >> 8 ) & 0xff ) << 0 ) | ( ( ( n_level >> 0 ) & 0xff ) << 8 );
	}

	for( int n_level = 0; n_level < MAX_LEVEL; n_level++ )
	{
		for( int n_level2 = 0; n_level2 < MAX_LEVEL; n_level2++ )
		{
			/* add transparency */
			int n_shaded = ( n_level + n_level2 );
			if( n_shaded > MAX_LEVEL - 1 )
			{
				n_shaded = MAX_LEVEL - 1;
			}
			p_n_redaddtrans[ ( n_level * MAX_LEVEL ) | n_level2 ] = n_shaded;
			p_n_greenaddtrans[ ( n_level * MAX_LEVEL ) | n_level2 ] = n_shaded << 5;
			p_n_blueaddtrans[ ( n_level * MAX_LEVEL ) | n_level2 ] = n_shaded << 10;

			/* sub transparency */
			n_shaded = ( n_level - n_level2 );
			if( n_shaded < 0 )
			{
				n_shaded = 0;
			}
			p_n_redsubtrans[ ( n_level * MAX_LEVEL ) | n_level2 ] = n_shaded;
			p_n_greensubtrans[ ( n_level * MAX_LEVEL ) | n_level2 ] = n_shaded << 5;
			p_n_bluesubtrans[ ( n_level * MAX_LEVEL ) | n_level2 ] = n_shaded << 10;
		}
	}

	save_item(NAME(m_packet.n_entry));
	save_item(NAME(n_gpu_buffer_offset));
	save_item(NAME(n_vramx));
	save_item(NAME(n_vramy));
	save_item(NAME(n_twy));
	save_item(NAME(n_twx));
	save_item(NAME(n_tww));
	save_item(NAME(n_drawarea_x1));
	save_item(NAME(n_drawarea_y1));
	save_item(NAME(n_drawarea_x2));
	save_item(NAME(n_drawarea_y2));
	save_item(NAME(n_horiz_disstart));
	save_item(NAME(n_horiz_disend));
	save_item(NAME(n_vert_disstart));
	save_item(NAME(n_vert_disend));
	save_item(NAME(b_reverseflag));
	save_item(NAME(n_drawoffset_x));
	save_item(NAME(n_drawoffset_y));
	save_item(NAME(m_n_displaystartx));
	save_item(NAME(n_displaystarty));
	save_item(NAME(n_gpustatus));
	save_item(NAME(n_gpuinfo));
	save_item(NAME(n_lightgun_x));
	save_item(NAME(n_lightgun_y));
	save_item(NAME(m_n_tx));
	save_item(NAME(m_n_ty));
	save_item(NAME(n_abr));
	save_item(NAME(n_tp));
	save_item(NAME(n_ix));
	save_item(NAME(n_iy));
	save_item(NAME(n_ti));
	save_item(NAME(m_draw_stp));
	save_item(NAME(m_check_stp));
}

void psxgpu_device::device_post_load()
{
	updatevisiblearea();
}

uint32_t psxgpu_device::update_screen(screen_device &screen, bitmap_rgb32 &bitmap, const rectangle &cliprect)
{
	if (gpu_active())
	{
		return gpu_update_screen(bitmap);
	}

	uint32_t n_x;
	uint32_t n_y;
	int n_top;
	int n_line;
	int n_lines;
	int n_left;
	int n_column;
	int n_columns;
	int n_displaystartx;
	int n_overscantop;
	int n_overscanleft;

#if PSXGPU_DEBUG_VIEWER
	if( DebugMeshDisplay( bitmap, cliprect ) )
	{
		return 0;
	}
	if( DebugTextureDisplay( bitmap ) )
	{
		return 0;
	}
#endif

	if( ( n_gpustatus & ( 1 << 0x17 ) ) != 0 )
	{
		/* todo: only draw to necessary area */
		bitmap.fill(0, cliprect);
	}
	else
	{
		if( b_reverseflag )
		{
			n_displaystartx = ( 1023 - m_n_displaystartx );
			/* todo: make this flip the screen, in the meantime.. */
			n_displaystartx -= ( n_screenwidth - 1 );
		}
		else
		{
			n_displaystartx = m_n_displaystartx;
		}

		if( ( n_gpustatus & ( 1 << 0x14 ) ) != 0 )
		{
			/* pal */
			n_overscantop = 0x23;
			n_overscanleft = 0x27e;
		}
		else
		{
			/* ntsc */
			n_overscantop = 0x10;
			n_overscanleft = 0x260;
		}

		n_top = (int32_t)n_vert_disstart - n_overscantop;
		n_lines = (int32_t)n_vert_disend - (int32_t)n_vert_disstart;
		if( n_top < 0 )
		{
			n_y = -n_top;
			n_lines += n_top;
		}
		else
		{
			n_y = 0;

			/* draw top border */
			rectangle clip(cliprect.left(), cliprect.right(), cliprect.top(), n_top);
			bitmap.fill(0, clip);
		}
		if( ( n_gpustatus & ( 1 << 0x16 ) ) != 0 )
		{
			/* interlaced */
			n_lines *= 2;
		}
		if( n_lines > n_screenheight - ( n_y + n_top ) )
		{
			n_lines = n_screenheight - ( n_y + n_top );
		}
		else
		{
			/* draw bottom border */
			rectangle clip(cliprect.left(), cliprect.right(), n_y + n_top + n_lines, cliprect.bottom());
			bitmap.fill(0, clip);
		}

		n_left = ( ( (int32_t)n_horiz_disstart - n_overscanleft ) * (int32_t)n_screenwidth ) / 2560;
		n_columns = ( ( ( (int32_t)n_horiz_disend - n_horiz_disstart ) * (int32_t)n_screenwidth ) / 2560 );
		
		// MAMEFX 2022-11-26 fix for screen cut off on the left (MT02957) (mame PR#7624)- hack by leejeonghun
		// added by chamcham425 aka Zansword(2023-04-02)
		if (n_left > n_screenwidth - n_columns)
			n_left = n_screenwidth - n_columns;
		// MAMEFX end	
		
		if( n_left < 0 )
		{
			n_x = -n_left;
			n_columns += n_left;
		}
		else
		{
			n_x = 0;

			/* draw left border */
			rectangle clip(cliprect.left(), n_x + n_left, cliprect.top(), cliprect.bottom());
			bitmap.fill(0, clip);
		}
		if( n_columns > n_screenwidth - ( n_x + n_left ) )
		{
			n_columns = n_screenwidth - ( n_x + n_left );
		}
		else
		{
			/* draw right border */
			rectangle clip(n_x + n_left + n_columns, cliprect.right(), cliprect.top(), cliprect.bottom());
			bitmap.fill(0, clip);
		}

		if( ( n_gpustatus & ( 1 << 0x15 ) ) != 0 )
		{
			/* 24bit */
			n_line = n_lines;
			while( n_line > 0 )
			{
				uint16_t *p_n_src = p_p_vram[ n_y + n_displaystarty ] + 3 * n_x + n_displaystartx;
				uint32_t *p_n_dest = &bitmap.pix(n_y + n_top, n_x + n_left);

				n_column = n_columns;
				while( n_column > 0 )
				{
					uint32_t n_g0r0 = *( p_n_src++ );
					uint32_t n_r1b0 = *( p_n_src++ );
					uint32_t n_b1g1 = *( p_n_src++ );

					*( p_n_dest++ ) = p_n_g0r0[ n_g0r0 ] | p_n_b0[ n_r1b0 ];
					n_column--;
					if( n_column > 0 )
					{
						*( p_n_dest++ ) = p_n_r1[ n_r1b0 ] | p_n_b1g1[ n_b1g1 ];
						n_column--;
					}
				}
				n_y++;
				n_line--;
			}
		}
		else
		{
			/* 15bit */
			n_line = n_lines;
			while( n_line > 0 )
			{
				draw_scanline16( bitmap, n_x + n_left, n_y + n_top, n_columns, p_p_vram[ ( n_y + n_displaystarty ) & 1023 ] + n_x + n_displaystartx, pens() );
				n_y++;
				n_line--;
			}
		}
	}
	return 0;
}

#define WRITE_PIXEL( p ) \
	{ \
		if( !m_check_stp || ( *( p_vram ) & 0x8000 ) == 0 ) \
		{ \
			if( m_draw_stp ) \
				*( p_vram ) = ( p ) | 0x8000; \
			else \
				*( p_vram ) = p; \
		} \
	}

/*
type 1
f  e| d| c  b| a  9| 8  7| 6  5| 4| 3  2  1  0
    |ti|     |   tp|  abr|   ty|  |         tx
*/

/*
type 2
f  e| d  c| b| a  9| 8  7| 6  5| 4| 3  2  1  0
    |iy|ix|ty|     |   tp|  abr|ty|         tx
*/

void psxgpu_device::decode_tpage( uint32_t tpage )
{
	if( m_n_gputype == 2 )
	{
		n_gpustatus = ( n_gpustatus & 0xffff7800 ) | ( tpage & 0x7ff ) | ( ( tpage & 0x800 ) << 4 );

		m_n_tx = ( tpage & 0x0f ) << 6;
		m_n_ty = ( ( tpage & 0x10 ) << 4 ) | ( ( tpage & 0x800 ) >> 2 );
		n_abr = ( tpage & 0x60 ) >> 5;
		n_tp = ( tpage & 0x180 ) >> 7;
		n_ix = ( tpage & 0x1000 ) >> 12;
		n_iy = ( tpage & 0x2000 ) >> 13;
		n_ti = 0;
		if( ( tpage & ~0x39ff ) != 0 )
		{
			LOG("not handled: draw mode %08x\n", tpage & ~0x39ff);
		}
		if( n_tp == 3 )
		{
			logerror("not handled: tp == 3\n");
		}
	}
	else
	{
		// TODO: confirm status bits on real type 1 gpu
		n_gpustatus = ( n_gpustatus & 0xffffe000 ) | ( tpage & 0x1fff );

		m_n_tx = ( tpage & 0x0f ) << 6;
		m_n_ty = ( ( tpage & 0x60 ) << 3 );
		n_abr = ( tpage & 0x180 ) >> 7;
		n_tp = ( tpage & 0x600 ) >> 9;
		n_ti = ( tpage & 0x2000 ) >> 13;
		n_ix = 0;
		n_iy = 0;
		if( ( tpage & ~0x27ef ) != 0 )
		{
			LOG("not handled: draw mode %08x\n", tpage & ~0x27ef);
		}
		if( n_tp == 3 )
		{
			logerror("not handled: tp == 3\n");
		}
		else if( n_tp == 2 && n_ti != 0 )
		{
			logerror("not handled: interleaved 15 bit texture\n");
		}
	}
}

#define SPRITESETUP \
	int n_dv; \
	if( n_iy != 0 ) \
	{ \
		n_dv = -1; \
	} \
	else \
	{ \
		n_dv = 1; \
	} \
	int n_du; \
	if( n_ix != 0 ) \
	{ \
		n_du = -1; \
	} \
	else \
	{ \
		n_du = 1; \
	}

#define TRANSPARENCYSETUP \
	uint16_t *p_n_f = p_n_f1; \
	uint16_t *p_n_redb = p_n_redb1; \
	uint16_t *p_n_greenb = p_n_greenb1; \
	uint16_t *p_n_blueb = p_n_blueb1; \
	uint16_t *p_n_redtrans = p_n_redaddtrans; \
	uint16_t *p_n_greentrans = p_n_greenaddtrans; \
	uint16_t *p_n_bluetrans = p_n_blueaddtrans; \
	\
	switch( n_cmd & 0x02 ) \
	{ \
	case 0x02: \
		switch( n_abr ) \
		{ \
		case 0x00: \
			p_n_f = p_n_f05; \
			p_n_redb = p_n_redb05; \
			p_n_greenb = p_n_greenb05; \
			p_n_blueb = p_n_blueb05; \
			p_n_redtrans = p_n_redaddtrans; \
			p_n_greentrans = p_n_greenaddtrans; \
			p_n_bluetrans = p_n_blueaddtrans; \
			LOGMASKED(LOG_TRANSPARENCY, "Transparency Mode: 0.5*B + 0.5*F\n"); \
			break; \
		case 0x01: \
			p_n_f = p_n_f1; \
			p_n_redb = p_n_redb1; \
			p_n_greenb = p_n_greenb1; \
			p_n_blueb = p_n_blueb1; \
			p_n_redtrans = p_n_redaddtrans; \
			p_n_greentrans = p_n_greenaddtrans; \
			p_n_bluetrans = p_n_blueaddtrans; \
			LOGMASKED(LOG_TRANSPARENCY, "Transparency Mode: 1.0*B + 1.0*F\n"); \
			break; \
		case 0x02: \
			p_n_f = p_n_f1; \
			p_n_redb = p_n_redb1; \
			p_n_greenb = p_n_greenb1; \
			p_n_blueb = p_n_blueb1; \
			p_n_redtrans = p_n_redsubtrans; \
			p_n_greentrans = p_n_greensubtrans; \
			p_n_bluetrans = p_n_bluesubtrans; \
			LOGMASKED(LOG_TRANSPARENCY, "Transparency Mode: 1.0*B - 1.0*F\n"); \
			break; \
		case 0x03: \
			p_n_f = p_n_f025; \
			p_n_redb = p_n_redb1; \
			p_n_greenb = p_n_greenb1; \
			p_n_blueb = p_n_blueb1; \
			p_n_redtrans = p_n_redaddtrans; \
			p_n_greentrans = p_n_greenaddtrans; \
			p_n_bluetrans = p_n_blueaddtrans; \
			LOGMASKED(LOG_TRANSPARENCY, "Transparency Mode: 1.0*B + 0.25*F\n"); \
			break; \
		} \
		break; \
	}

#define SOLIDSETUP \
	TRANSPARENCYSETUP

#define TEXTURESETUP \
	int n_tx = m_n_tx; \
	int n_ty = m_n_ty; \
	uint16_t *p_clut = p_p_vram[ n_cluty ] + n_clutx; \
	switch( n_tp ) \
	{ \
	case 0: \
		n_tx += n_twx >> 2; \
		n_ty += n_twy; \
		break; \
	case 1: \
		n_tx += n_twx >> 1; \
		n_ty += n_twy; \
		break; \
	case 2: \
		n_tx += n_twx >> 0; \
		n_ty += n_twy; \
		break; \
	} \
	TRANSPARENCYSETUP

#define FLATPOLYGONUPDATE
#define FLATRECTANGEUPDATE
#define GOURAUDPOLYGONUPDATE \
	n_r.d += n_dr; \
	n_g.d += n_dg; \
	n_b.d += n_db;

#define SOLIDFILL( PIXELUPDATE ) \
	if( n_distance > ( (int32_t)n_drawarea_x2 - drawx ) + 1 ) \
	{ \
		n_distance = ( n_drawarea_x2 - drawx ) + 1; \
	} \
	uint16_t *p_vram = p_p_vram[ drawy ] + drawx; \
	\
	switch( n_cmd & 0x02 ) \
	{ \
	case 0x00: \
		/* transparency off */ \
		while( n_distance > 0 ) \
		{ \
			WRITE_PIXEL( \
				p_n_redshade[ MID_LEVEL | n_r.w.h ] | \
				p_n_greenshade[ MID_LEVEL | n_g.w.h ] | \
				p_n_blueshade[ MID_LEVEL | n_b.w.h ] ) \
			p_vram++; \
			PIXELUPDATE \
			n_distance--; \
		} \
		break; \
	case 0x02: \
		/* transparency on */ \
		while( n_distance > 0 ) \
		{ \
			WRITE_PIXEL( \
				p_n_redtrans[ p_n_f[ MID_LEVEL | n_r.w.h ] | p_n_redb[ *( p_vram ) ] ] | \
				p_n_greentrans[ p_n_f[ MID_LEVEL | n_g.w.h ] | p_n_greenb[ *( p_vram ) ] ] | \
				p_n_bluetrans[ p_n_f[ MID_LEVEL | n_b.w.h ] | p_n_blueb[ *( p_vram ) ] ] ) \
			p_vram++; \
			PIXELUPDATE \
			n_distance--; \
		} \
		break; \
	}

#define FLATTEXTUREDPOLYGONUPDATE \
	n_u.d += n_du; \
	n_v.d += n_dv;

#define GOURAUDTEXTUREDPOLYGONUPDATE \
	n_r.d += n_dr; \
	n_g.d += n_dg; \
	n_b.d += n_db; \
	n_u.d += n_du; \
	n_v.d += n_dv;

#define FLATTEXTUREDRECTANGLEUPDATE \
	n_u += n_du;

#define TEXTURE_LOOP \
	while( n_distance > 0 ) \
	{
#define TEXTURE_ENDLOOP \
	}

#define TEXTURE4BIT( TXV, TXU ) \
	TEXTURE_LOOP \
		uint16_t n_bgr = p_clut[ ( *( p_p_vram[ n_ty + TXV ] + n_tx + ( TXU >> 2 ) ) >> ( ( TXU & 0x03 ) << 2 ) ) & 0x0f ];

#define TEXTURE8BIT( TXV, TXU ) \
	TEXTURE_LOOP \
		uint16_t n_bgr = p_clut[ ( *( p_p_vram[ n_ty + TXV ] + n_tx + ( TXU >> 1 ) ) >> ( ( TXU & 0x01 ) << 3 ) ) & 0xff ];

#define TEXTURE15BIT( TXV, TXU ) \
	TEXTURE_LOOP \
		uint16_t n_bgr = *( p_p_vram[ n_ty + TXV ] + n_tx + TXU );

#define TEXTUREWINDOW4BIT( TXV, TXU ) TEXTURE4BIT( ( TXV & n_twh ), ( TXU & n_tww ) )
#define TEXTUREWINDOW8BIT( TXV, TXU ) TEXTURE8BIT( ( TXV & n_twh ), ( TXU & n_tww ) )
#define TEXTUREWINDOW15BIT( TXV, TXU ) TEXTURE15BIT( ( TXV & n_twh ), ( TXU & n_tww ) )

#define TEXTUREINTERLEAVED4BIT( TXV, TXU ) \
	TEXTURE_LOOP \
		int n_xi = ( ( TXU >> 2 ) & ~0x3c ) + ( ( TXV << 2 ) & 0x3c ); \
		int n_yi = ( TXV & ~0xf ) + ( ( TXU >> 4 ) & 0xf ); \
		uint16_t n_bgr = p_clut[ ( *( p_p_vram[ n_ty + n_yi ] + n_tx + n_xi ) >> ( ( TXU & 0x03 ) << 2 ) ) & 0x0f ];

#define TEXTUREINTERLEAVED8BIT( TXV, TXU ) \
	TEXTURE_LOOP \
		int n_xi = ( ( TXU >> 1 ) & ~0x78 ) + ( ( TXU << 2 ) & 0x40 ) + ( ( TXV << 3 ) & 0x38 ); \
		int n_yi = ( TXV & ~0x7 ) + ( ( TXU >> 5 ) & 0x7 ); \
		uint16_t n_bgr = p_clut[ ( *( p_p_vram[ n_ty + n_yi ] + n_tx + n_xi ) >> ( ( TXU & 0x01 ) << 3 ) ) & 0xff ];

#define TEXTUREINTERLEAVED15BIT( TXV, TXU ) \
	TEXTURE_LOOP \
		int n_xi = TXU; \
		int n_yi = TXV; \
		uint16_t n_bgr = *( p_p_vram[ n_ty + n_yi ] + n_tx + n_xi );

#define TEXTUREWINDOWINTERLEAVED4BIT( TXV, TXU ) TEXTUREINTERLEAVED4BIT( ( TXV & n_twh ), ( TXU & n_tww ) )
#define TEXTUREWINDOWINTERLEAVED8BIT( TXV, TXU ) TEXTUREINTERLEAVED8BIT( ( TXV & n_twh ), ( TXU & n_tww ) )
#define TEXTUREWINDOWINTERLEAVED15BIT( TXV, TXU ) TEXTUREINTERLEAVED15BIT( ( TXV & n_twh ), ( TXU & n_tww ) )

#define SHADEDPIXEL( PIXELUPDATE ) \
		if( n_bgr != 0 ) \
		{ \
			WRITE_PIXEL( \
				p_n_redshade[ p_n_redlevel[ n_bgr ] | n_r.w.h ] | \
				p_n_greenshade[ p_n_greenlevel[ n_bgr ] | n_g.w.h ] | \
				p_n_blueshade[ p_n_bluelevel[ n_bgr ] | n_b.w.h ] | \
				( n_bgr & 0x8000 ) ) \
		} \
		p_vram++; \
		PIXELUPDATE \
		n_distance--; \
	TEXTURE_ENDLOOP

#define TRANSPARENTPIXEL( PIXELUPDATE ) \
		if( n_bgr != 0 ) \
		{ \
			if( ( n_bgr & 0x8000 ) != 0 ) \
			{ \
				WRITE_PIXEL( \
					p_n_redtrans[ p_n_f[ p_n_redlevel[ n_bgr ] | n_r.w.h ] | p_n_redb[ *( p_vram ) ] ] | \
					p_n_greentrans[ p_n_f[ p_n_greenlevel[ n_bgr ] | n_g.w.h ] | p_n_greenb[ *( p_vram ) ] ] | \
					p_n_bluetrans[ p_n_f[ p_n_bluelevel[ n_bgr ] | n_b.w.h ] | p_n_blueb[ *( p_vram ) ] ] | \
					0x8000 ) \
			} \
			else \
			{ \
				WRITE_PIXEL( \
					p_n_redshade[ p_n_redlevel[ n_bgr ] | n_r.w.h ] | \
					p_n_greenshade[ p_n_greenlevel[ n_bgr ] | n_g.w.h ] | \
					p_n_blueshade[ p_n_bluelevel[ n_bgr ] | n_b.w.h ] ) \
			} \
		} \
		p_vram++; \
		PIXELUPDATE \
		n_distance--; \
	TEXTURE_ENDLOOP

#define TEXTUREFILL( PIXELUPDATE, TXU, TXV ) \
	if( n_distance > ( (int32_t)n_drawarea_x2 - drawx ) + 1 ) \
	{ \
		n_distance = ( n_drawarea_x2 - drawx ) + 1; \
	} \
	uint16_t *p_vram = p_p_vram[ drawy ] + drawx; \
	\
	if( n_ti != 0 ) \
	{ \
		/* interleaved texture */ \
		if( n_twh != 255 || \
			n_tww != 255 || \
			n_twx != 0 || \
			n_twy != 0 ) \
		{ \
			/* texture window */ \
			switch( n_cmd & 0x02 ) \
			{ \
			case 0x00: \
				/* shading */ \
				switch( n_tp ) \
				{ \
				case 0: \
					/* 4 bit clut */ \
					TEXTUREWINDOWINTERLEAVED4BIT( TXV, TXU ) \
					SHADEDPIXEL( PIXELUPDATE ) \
					break; \
				case 1: \
					/* 8 bit clut */ \
					TEXTUREWINDOWINTERLEAVED8BIT( TXV, TXU ) \
					SHADEDPIXEL( PIXELUPDATE ) \
					break; \
				case 2: \
					/* 15 bit */ \
					TEXTUREWINDOWINTERLEAVED15BIT( TXV, TXU ) \
					SHADEDPIXEL( PIXELUPDATE ) \
					break; \
				} \
				break; \
			case 0x02: \
				/* semi transparency */ \
				switch( n_tp ) \
				{ \
				case 0: \
					/* 4 bit clut */ \
					TEXTUREWINDOWINTERLEAVED4BIT( TXV, TXU ) \
					TRANSPARENTPIXEL( PIXELUPDATE ) \
					break; \
				case 1: \
					/* 8 bit clut */ \
					TEXTUREWINDOWINTERLEAVED8BIT( TXV, TXU ) \
					TRANSPARENTPIXEL( PIXELUPDATE ) \
					break; \
				case 2: \
					/* 15 bit */ \
					TEXTUREWINDOWINTERLEAVED15BIT( TXV, TXU ) \
					TRANSPARENTPIXEL( PIXELUPDATE ) \
					break; \
				} \
				break; \
			} \
		} \
		else \
		{ \
			/* no texture window */ \
			switch( n_cmd & 0x02 ) \
			{ \
			case 0x00: \
				/* shading */ \
				switch( n_tp ) \
				{ \
				case 0: \
					/* 4 bit clut */ \
					TEXTUREINTERLEAVED4BIT( TXV, TXU ) \
					SHADEDPIXEL( PIXELUPDATE ) \
					break; \
				case 1: \
					/* 8 bit clut */ \
					TEXTUREINTERLEAVED8BIT( TXV, TXU ) \
					SHADEDPIXEL( PIXELUPDATE ) \
					break; \
				case 2: \
					/* 15 bit */ \
					TEXTUREINTERLEAVED15BIT( TXV, TXU ) \
					SHADEDPIXEL( PIXELUPDATE ) \
					break; \
				} \
				break; \
			case 0x02: \
				/* semi transparency */ \
				switch( n_tp ) \
				{ \
				case 0: \
					/* 4 bit clut */ \
					TEXTUREINTERLEAVED4BIT( TXV, TXU ) \
					TRANSPARENTPIXEL( PIXELUPDATE ) \
					break; \
				case 1: \
					/* 8 bit clut */ \
					TEXTUREINTERLEAVED8BIT( TXV, TXU ) \
					TRANSPARENTPIXEL( PIXELUPDATE ) \
					break; \
				case 2: \
					/* 15 bit */ \
					TEXTUREINTERLEAVED15BIT( TXV, TXU ) \
					TRANSPARENTPIXEL( PIXELUPDATE ) \
					break; \
				} \
				break; \
			} \
		} \
	} \
	else \
	{ \
		/* standard texture */ \
		if( n_twh != 255 || \
			n_tww != 255 || \
			n_twx != 0 || \
			n_twy != 0 ) \
		{ \
			/* texture window */ \
			switch( n_cmd & 0x02 ) \
			{ \
			case 0x00: \
				/* shading */ \
				switch( n_tp ) \
				{ \
				case 0: \
					/* 4 bit clut */ \
					TEXTUREWINDOW4BIT( TXV, TXU ) \
					SHADEDPIXEL( PIXELUPDATE ) \
					break; \
				case 1: \
					/* 8 bit clut */ \
					TEXTUREWINDOW8BIT( TXV, TXU ) \
					SHADEDPIXEL( PIXELUPDATE ) \
					break; \
				case 2: \
					/* 15 bit */ \
					TEXTUREWINDOW15BIT( TXV, TXU ) \
					SHADEDPIXEL( PIXELUPDATE ) \
					break; \
				} \
				break; \
			case 0x02: \
				/* semi transparency */ \
				switch( n_tp ) \
				{ \
				case 0: \
					/* 4 bit clut */ \
					TEXTUREWINDOW4BIT( TXV, TXU ) \
					TRANSPARENTPIXEL( PIXELUPDATE ) \
					break; \
				case 1: \
					/* 8 bit clut */ \
					TEXTUREWINDOW8BIT( TXV, TXU ) \
					TRANSPARENTPIXEL( PIXELUPDATE ) \
					break; \
				case 2: \
					/* 15 bit */ \
					TEXTUREWINDOW15BIT( TXV, TXU ) \
					TRANSPARENTPIXEL( PIXELUPDATE ) \
					break; \
				} \
				break; \
			} \
		} \
		else \
		{ \
			/* no texture window */ \
			switch( n_cmd & 0x02 ) \
			{ \
			case 0x00: \
				/* shading */ \
				switch( n_tp ) \
				{ \
				case 0: \
					TEXTURE4BIT( TXV, TXU ) \
					SHADEDPIXEL( PIXELUPDATE ) \
					break; \
				case 1: \
					/* 8 bit clut */ \
					TEXTURE8BIT( TXV, TXU ) \
					SHADEDPIXEL( PIXELUPDATE ) \
					break; \
				case 2: \
					/* 15 bit */ \
					TEXTURE15BIT( TXV, TXU ) \
					SHADEDPIXEL( PIXELUPDATE ) \
					break; \
				} \
				break; \
			case 0x02: \
				/* semi transparency */ \
				switch( n_tp ) \
				{ \
				case 0: \
					/* 4 bit clut */ \
					TEXTURE4BIT( TXV, TXU ) \
					TRANSPARENTPIXEL( PIXELUPDATE ) \
					break; \
				case 1: \
					/* 8 bit clut */ \
					TEXTURE8BIT( TXV, TXU ) \
					TRANSPARENTPIXEL( PIXELUPDATE ) \
					break; \
				case 2: \
					/* 15 bit */ \
					TEXTURE15BIT( TXV, TXU ) \
					TRANSPARENTPIXEL( PIXELUPDATE ) \
					break; \
				} \
				break; \
			} \
		} \
	}

#define GET_COORD( a ) \
	a.sw.l = S11_COORD_X( a ); \
	a.sw.h = S11_COORD_Y( a );

static inline int CullVertex( int a, int b )
{
	int d = a - b;
	if( d < -1023 || d > 1023 )
	{
		return 1;
	}

	return 0;
}

#define CULLPOINT( PacketType, p1, p2 ) \
( \
	CullVertex( COORD_Y( m_packet.PacketType.vertex[ p1 ].n_coord ), COORD_Y( m_packet.PacketType.vertex[ p2 ].n_coord ) ) || \
	CullVertex( COORD_X( m_packet.PacketType.vertex[ p1 ].n_coord ), COORD_X( m_packet.PacketType.vertex[ p2 ].n_coord ) ) \
)

#define CULLTRIANGLE( PacketType, start ) \
( \
	CULLPOINT( PacketType, start, start + 1 ) || CULLPOINT( PacketType, start + 1, start + 2 ) || CULLPOINT( PacketType, start + 2, start ) \
)

#define FINDTOPLEFT( PacketType ) \
	for( int n_point = 0; n_point < n_points; n_point++ ) \
	{ \
		GET_COORD( m_packet.PacketType.vertex[ n_point ].n_coord ); \
	} \
	\
	const int *p_n_rightpointlist; \
	const int *p_n_leftpointlist; \
	int n_leftpoint = 0; \
	if( n_points == 4 ) \
	{ \
		if( CULLTRIANGLE( PacketType, 0 ) ) \
		{ \
			if( CULLTRIANGLE( PacketType, 1 ) ) \
			{ \
				return; \
			} \
			\
			p_n_rightpointlist = m_p_n_nextpointlist4b; \
			p_n_leftpointlist = m_p_n_prevpointlist4b; \
			n_leftpoint++; \
		} \
		else if( CULLTRIANGLE( PacketType, 1 ) ) \
		{ \
			p_n_rightpointlist = m_p_n_nextpointlist3; \
			p_n_leftpointlist = m_p_n_prevpointlist3; \
			n_points--; \
		} \
		else \
		{ \
			p_n_rightpointlist = m_p_n_nextpointlist4; \
			p_n_leftpointlist = m_p_n_prevpointlist4; \
		} \
	} \
	else if( CULLTRIANGLE( PacketType, 0 ) ) \
	{ \
		return; \
	} \
	else \
	{ \
		p_n_rightpointlist = m_p_n_nextpointlist3; \
		p_n_leftpointlist = m_p_n_prevpointlist3; \
	} \
	\
	for( int n_point = n_leftpoint + 1; n_point < n_points; n_point++ ) \
	{ \
		if( COORD_Y( m_packet.PacketType.vertex[ n_point ].n_coord ) < COORD_Y( m_packet.PacketType.vertex[ n_leftpoint ].n_coord ) || \
			( COORD_Y( m_packet.PacketType.vertex[ n_point ].n_coord ) == COORD_Y( m_packet.PacketType.vertex[ n_leftpoint ].n_coord ) && \
			COORD_X( m_packet.PacketType.vertex[ n_point ].n_coord ) < COORD_X( m_packet.PacketType.vertex[ n_leftpoint ].n_coord ) ) ) \
		{ \
			n_leftpoint = n_point; \
		} \
	} \
	int n_rightpoint = n_leftpoint;

//**************************************************************************
//  GPU-accelerated rendering (Phase 2, extended post-Phase-3 2026-09-17)
//
//  Every primitive that can end up visible on screen is routed to the GPU
//  target when gpu_active(): the four polygon types, rectangles/sprites
//  (FlatRectangle[8x8/16x16]/FlatTexturedRectangle/Sprite8x8/16x16), Dot/
//  TexturedDot, Monochrome/GouraudLine, the VRAM-fill (0x02) and CPU-to-
//  VRAM image transfer (0xA0) commands, and MoveImage (VRAM-to-VRAM copy,
//  0x80). None of these write to the CPU-side p_vram buffer's *visible*
//  effect once GPU-routed - update_screen() no longer scans VRAM for
//  display in that case, see gpu_update_screen() - though 0xA0 still
//  writes p_vram too, deliberately, since it doubles as this frame's
//  upload_vram() texture source (see gpu_submit_image_stamp()'s comment).
//  Originally only the four polygon primitives were routed here (see git
//  history) - ghosting bugs found via gdarius2 (rectangle/sprite fill/
//  clear) and starswep (2D-only content relying on 0xA0 image transfers)
//  are what closed the remaining gaps; see CLAUDE.md's "Post-Phase-3
//  fixes" note.
//**************************************************************************

osd::gpu_blend_mode psxgpu_device::gpu_blend_mode_for( uint8_t n_cmd ) const
{
	if( ( n_cmd & 0x02 ) == 0 )
	{
		return osd::gpu_blend_mode::NONE;
	}
	switch( n_abr )
	{
	case 0: return osd::gpu_blend_mode::HALF_ADD;
	case 1: return osd::gpu_blend_mode::ADD;
	case 2: return osd::gpu_blend_mode::SUBTRACT;
	case 3: return osd::gpu_blend_mode::ADD_QUARTER;
	default: return osd::gpu_blend_mode::NONE;
	}
}

// The real PS1 GPU clips all primitives to a settable draw-area rectangle
// (n_drawarea_x1/y1/x2/y2, used extensively by the CPU scanline rasterizer's
// clipping logic elsewhere in this file) - the GPU path ignored this
// entirely until now, which large edge-of-screen polygons (ground/sky
// backdrops - exactly the geometry most likely to extend beyond whatever
// the current draw area is) are the most exposed to: as the draw area
// changes from frame to frame, unclipped polygons bleed into/overwrite
// regions they shouldn't, on top of the persistent framebuffer this target
// now uses - visible as flicker. Cached the same way as the texture page
// above, since consecutive polygons usually share one draw area.
void psxgpu_device::gpu_maybe_set_clip_rect()
{
	if( m_gpu_drawarea_cached &&
		n_drawarea_x1 == m_gpu_last_drawarea_x1 && n_drawarea_y1 == m_gpu_last_drawarea_y1 &&
		n_drawarea_x2 == m_gpu_last_drawarea_x2 && n_drawarea_y2 == m_gpu_last_drawarea_y2 )
	{
		return;
	}

	gpu_queued_cmd cmd;
	cmd.kind = gpu_queued_cmd::kind_t::CLIP;
	cmd.clip_x1 = (int)n_drawarea_x1 * gpu_scale();
	cmd.clip_y1 = (int)n_drawarea_y1 * gpu_scale();
	cmd.clip_x2 = (int)( n_drawarea_x2 + 1 ) * gpu_scale() - 1;
	cmd.clip_y2 = (int)( n_drawarea_y2 + 1 ) * gpu_scale() - 1;
	m_gpu_queue.push_back( std::move( cmd ) );

	m_gpu_drawarea_cached = true;
	m_gpu_last_drawarea_x1 = n_drawarea_x1;
	m_gpu_last_drawarea_y1 = n_drawarea_y1;
	m_gpu_last_drawarea_x2 = n_drawarea_x2;
	m_gpu_last_drawarea_y2 = n_drawarea_y2;
}

// The real PS1 GPU silently discards any triangle with two vertices more
// than a fixed distance apart (MAME's software path: CULLTRIANGLE/CullVertex
// above, > 1023 in either axis on the raw native, pre-PGXP, pre-draw-offset
// coordinates). Games rely on this: a perspective-projected polygon that
// swings far past the camera plane simply vanishes instead of smearing
// across the screen. The GPU path used to skip the check entirely (it
// returned before the software cull ran) and drew those polygons at their
// full oversized extent - brvbladej covered its whole screen in a stretched
// texture that way. A quad is two triangles, (0,1,2) and (1,2,3), culled
// independently: bit 0 of the result = first triangle culled, bit 1 = second.
int psxgpu_device::gpu_polygon_cull_mask( const PAIR *n_coord, int n_points )
{
	auto tri_culled = [ n_coord ]( int a, int b, int c )
	{
		const int idx[ 3 ][ 2 ] = { { a, b }, { b, c }, { c, a } };
		for( auto &e : idx )
		{
			// Real hardware limits (and Beetle PSX HW's): >= 1024 apart in x,
			// >= 512 apart in y. MAME's own software path (CullVertex) uses
			// 1023 for both axes, which lets through vertically-oversized
			// triangles the hardware drops.
			int dx = S11_COORD_X( n_coord[ e[ 0 ] ] ) - S11_COORD_X( n_coord[ e[ 1 ] ] );
			int dy = S11_COORD_Y( n_coord[ e[ 0 ] ] ) - S11_COORD_Y( n_coord[ e[ 1 ] ] );
			if( dx >= 1024 || dx <= -1024 || dy >= 512 || dy <= -512 )
			{
				return true;
			}
		}
		return false;
	};
	int mask = tri_culled( 0, 1, 2 ) ? 1 : 0;
	if( n_points == 4 && tri_culled( 1, 2, 3 ) )
		mask |= 2;
	return mask;
}

void psxgpu_device::gpu_queue_triangle_pair( const osd::gpu_vertex &v0, const osd::gpu_vertex &v1, const osd::gpu_vertex &v2, const osd::gpu_vertex &v3, int n_points, bool textured, osd::gpu_blend_mode blend, bool filterable, int cull_mask )
{
	if( !( cull_mask & 1 ) )
	{
		gpu_queued_cmd cmd;
		cmd.kind = gpu_queued_cmd::kind_t::TRIANGLE;
		cmd.tri[ 0 ] = v0; cmd.tri[ 1 ] = v1; cmd.tri[ 2 ] = v2;
		cmd.textured = textured;
		cmd.filterable = filterable;
		cmd.blend = blend;
		m_gpu_queue.push_back( std::move( cmd ) );
	}

	if( n_points == 4 && !( cull_mask & 2 ) )
	{
		gpu_queued_cmd cmd2;
		cmd2.kind = gpu_queued_cmd::kind_t::TRIANGLE;
		cmd2.tri[ 0 ] = v1; cmd2.tri[ 1 ] = v2; cmd2.tri[ 2 ] = v3;
		cmd2.textured = textured;
		cmd2.filterable = filterable;
		cmd2.blend = blend;
		m_gpu_queue.push_back( std::move( cmd2 ) );
	}
}

void psxgpu_device::gpu_submit_triangle_pair( const osd::gpu_vertex &v0, const osd::gpu_vertex &v1, const osd::gpu_vertex &v2, const osd::gpu_vertex &v3, int n_points, bool textured, osd::gpu_blend_mode blend, bool filterable, int cull_mask )
{
	gpu_maybe_set_clip_rect();
	gpu_queue_triangle_pair( v0, v1, v2, v3, n_points, textured, blend, filterable, cull_mask );
}

// A few PS1 GPU commands (Fill Rectangle in VRAM/0x02, CPU-to-VRAM image
// transfer/0xA0) operate directly on VRAM and are specified to bypass the
// draw-area clip entirely - unlike every other primitive above, which is
// always clipped to it (see gpu_maybe_set_clip_rect()). Queues an explicit
// full-target CLIP command so the *next* queued triangle(s) aren't clipped,
// then invalidates the clip cache so whatever primitive runs after this one
// unconditionally restores the game's actual draw area - gpu_maybe_set_clip_
// rect()'s cache-hit fast path would otherwise wrongly assume the clip is
// still what it last cached and skip re-emitting it.
void psxgpu_device::gpu_force_no_clip()
{
	gpu_queued_cmd cmd;
	cmd.kind = gpu_queued_cmd::kind_t::CLIP;
	cmd.clip_x1 = 0;
	cmd.clip_y1 = 0;
	// Full target bounds, not just the currently-declared display size -
	// the target itself is sized to full VRAM height (see
	// gpu_update_screen()'s comment), and an absolute-VRAM command like
	// this one may legitimately target a currently-hidden buffer region
	// past n_screenheight (double buffering).
	cmd.clip_x2 = (int)( n_screenwidth * gpu_scale() ) - 1;
	cmd.clip_y2 = (int)( m_vram_height * gpu_scale() ) - 1;
	m_gpu_queue.push_back( std::move( cmd ) );
	m_gpu_drawarea_cached = false;
}

bool psxgpu_device::gpu_vertex_xyw( int entry_index, PAIR n_coord, float &out_x, float &out_y, float &out_w ) const
{
	if( m_gpu_pgxp_enabled && entry_index >= 0 && entry_index < 16 && m_packet_shadow[ entry_index ].valid )
	{
		const pgxp_word_shadow &s = m_packet_shadow[ entry_index ];
		out_x = ( s.x + n_drawoffset_x ) * gpu_scale();
		out_y = ( s.y + n_drawoffset_y ) * gpu_scale();
		out_w = s.w;
		return true;
	}

	// No direct shadow. With the vertex cache on, try to recover a precise
	// x/y from any other GTE-transformed vertex that landed on this same
	// integer pixel (Beetle's PGXP_GetVertex() fallback). The w it carries
	// is deliberately not used (Beetle sets valid_w = 0 here): returning
	// false makes the whole primitive drop to w = 1 while keeping this
	// precise x/y, subject to the 2D tolerance - see
	// gpu_resolve_polygon_pgxp().
	if( m_gpu_pgxp_vcache && m_cpu != nullptr )
	{
		float cx, cy, cw;
		if( m_cpu->pgxp_vertex_cache_query( S11_COORD_X( n_coord ), S11_COORD_Y( n_coord ), cx, cy, cw ) )
		{
			out_x = ( cx + n_drawoffset_x ) * gpu_scale();
			out_y = ( cy + n_drawoffset_y ) * gpu_scale();
			out_w = 1.0f;
			return false;
		}
	}

	out_x = (float)( S11_COORD_X( n_coord ) + n_drawoffset_x ) * gpu_scale();
	out_y = (float)( S11_COORD_Y( n_coord ) + n_drawoffset_y ) * gpu_scale();
	out_w = 1.0f;
	return false;
}

// A PGXP miss on even one vertex of a triangle (no shadow ever reached
// this word - e.g. the game recomputed it without going through a tracked
// GTE->GPR->RAM round trip, or an intervening unrelated store invalidated
// it) must not be allowed to mix that vertex's fallback w=1.0 with real,
// much larger shadowed-depth w values on the other two - the resulting
// huge per-vertex w disparity is exactly what produces severe "warp to a
// focal point" texture distortion (see CLAUDE.md Phase 4). So correction
// is all-or-nothing per primitive: if any vertex misses, every vertex in
// that primitive reverts to the plain integer-coordinate/w=1.0 path,
// matching pre-PGXP behavior for that primitive rather than partially
// correcting it.
void psxgpu_device::gpu_resolve_polygon_pgxp( const int *entry_index, const PAIR *n_coord, int n_points, osd::gpu_vertex *v ) const
{
	bool all_hit = true;
	for( int i = 0; i < n_points; i++ )
	{
		all_hit = gpu_vertex_xyw( entry_index[ i ], n_coord[ i ], v[ i ].x, v[ i ].y, v[ i ].w ) && all_hit;
	}

	if( !all_hit && m_gpu_pgxp_tolerance > -2 )
	{
		// Beetle PSX HW-style handling (its pgxp_2d_tol option): keep each
		// corrected vertex's sub-pixel x/y - so shared edges with
		// neighbouring, fully-corrected polygons stay welded instead of
		// snapping to integers and opening a seam - but force w=1 on the
		// whole primitive to avoid the mixed-depth "warp to a focal
		// point" problem described above. gpu_vertex_xyw() already left
		// missed vertices at their native coordinates. With a tolerance
		// set (>= 0), a corrected position that strayed further than that
		// many native pixels from the native one is treated as an
		// outlier and reverted.
		const float tol = m_gpu_pgxp_tolerance >= 0 ? (float)m_gpu_pgxp_tolerance * gpu_scale() : -1.0f;
		for( int i = 0; i < n_points; i++ )
		{
			if( tol >= 0.0f )
			{
				float nx = (float)( S11_COORD_X( n_coord[ i ] ) + n_drawoffset_x ) * gpu_scale();
				float ny = (float)( S11_COORD_Y( n_coord[ i ] ) + n_drawoffset_y ) * gpu_scale();
				if( fabsf( v[ i ].x - nx ) > tol || fabsf( v[ i ].y - ny ) > tol )
				{
					v[ i ].x = nx;
					v[ i ].y = ny;
				}
			}
			v[ i ].w = 1.0f;
		}
	}
	else if( !all_hit )
	{
		for( int i = 0; i < n_points; i++ )
		{
			v[ i ].x = (float)( S11_COORD_X( n_coord[ i ] ) + n_drawoffset_x ) * gpu_scale();
			v[ i ].y = (float)( S11_COORD_Y( n_coord[ i ] ) + n_drawoffset_y ) * gpu_scale();
			v[ i ].w = 1.0f;
		}
	}
}

bool psxgpu_device::gpu_submit_flat_polygon( int n_points )
{
	uint8_t n_cmd = BGR_C( m_packet.FlatPolygon.n_bgr );
	float r = BGR_R( m_packet.FlatPolygon.n_bgr ) / 255.0f;
	float g = BGR_G( m_packet.FlatPolygon.n_bgr ) / 255.0f;
	float b = BGR_B( m_packet.FlatPolygon.n_bgr ) / 255.0f;

	osd::gpu_vertex v[ 4 ];
	PAIR coords[ 4 ];
	int indices[ 4 ];
	for( int i = 0; i < n_points; i++ )
	{
		coords[ i ] = m_packet.FlatPolygon.vertex[ i ].n_coord;
		indices[ i ] = gpu_entry_index( m_packet.FlatPolygon.vertex[ i ].n_coord );
		v[ i ].r = r; v[ i ].g = g; v[ i ].b = b; v[ i ].a = 1.0f;
		v[ i ].u = 0.0f; v[ i ].v = 0.0f;
	}
	gpu_resolve_polygon_pgxp( indices, coords, n_points, v );

	gpu_submit_triangle_pair( v[ 0 ], v[ 1 ], v[ 2 ], n_points == 4 ? v[ 3 ] : v[ 2 ], n_points, false, gpu_blend_mode_for( n_cmd ), true, gpu_polygon_cull_mask( coords, n_points ) );
	return true;
}

bool psxgpu_device::gpu_submit_gouraud_polygon( int n_points )
{
	uint8_t n_cmd = BGR_C( m_packet.GouraudPolygon.vertex[ 0 ].n_bgr );

	osd::gpu_vertex v[ 4 ];
	PAIR coords[ 4 ];
	int indices[ 4 ];
	for( int i = 0; i < n_points; i++ )
	{
		coords[ i ] = m_packet.GouraudPolygon.vertex[ i ].n_coord;
		indices[ i ] = gpu_entry_index( m_packet.GouraudPolygon.vertex[ i ].n_coord );
		v[ i ].r = BGR_R( m_packet.GouraudPolygon.vertex[ i ].n_bgr ) / 255.0f;
		v[ i ].g = BGR_G( m_packet.GouraudPolygon.vertex[ i ].n_bgr ) / 255.0f;
		v[ i ].b = BGR_B( m_packet.GouraudPolygon.vertex[ i ].n_bgr ) / 255.0f;
		v[ i ].a = 1.0f;
		v[ i ].u = 0.0f; v[ i ].v = 0.0f;
	}
	gpu_resolve_polygon_pgxp( indices, coords, n_points, v );

	gpu_submit_triangle_pair( v[ 0 ], v[ 1 ], v[ 2 ], n_points == 4 ? v[ 3 ] : v[ 2 ], n_points, false, gpu_blend_mode_for( n_cmd ), true, gpu_polygon_cull_mask( coords, n_points ) );
	return true;
}

// Mirrors Beetle PSX HW's filter_exclude_sprite/filter_exclude_2d_polygon
// options: exclude_mode is 0 = disabled (no exclusion), 1 = exclude only
// opaque draws, 2 = exclude both opaque and semi-transparent draws (see
// osdepend.h's gpu_render_filter_exclude_sprite()/_2d_polygon()). Returns
// whether this draw should still be eligible for the user's texture
// filtering mode after applying that exclusion.
bool psxgpu_device::gpu_filter_eligible( int exclude_mode, osd::gpu_blend_mode blend ) const
{
	if( exclude_mode == 2 )
		return false;
	if( exclude_mode == 1 && blend == osd::gpu_blend_mode::NONE )
		return false;
	return true;
}

// Heuristic used by the "Exclude 2D Polygons from Filtering" option (see
// gpu_filter_eligible()) to catch 2D content (HUD/text/UI) that a game
// draws via the general polygon commands instead of the dedicated sprite/
// rectangle ones - which the sprite-exclusion option above can't see at
// all. Two conditions, both checked against the *raw*, pre-drawoffset/
// pre-PGXP/pre-scale packet data (not the resolved gpu_vertex x/y, which
// PGXP may have nudged into not-quite-axis-aligned floats even for a
// visually flat 2D element):
//   - the quad is an unrotated, screen-axis-aligned rectangle (matches the
//     v0/v1 one edge, v2/v3 opposite edge winding gpu_queue_triangle_pair()
//     expects for a 4-point primitive)
//   - its screen-space size exactly matches its source texture rectangle's
//     size in texels (a 1:1, unscaled blit) - real 3D geometry essentially
//     never lands on an exact integer match here, while a 2D element
//     blitted flat onto the screen reliably does
// Like Beetle's own version of this heuristic, it's approximate - an
// unrotated, unscaled 3D surface (e.g. a flat wall tile viewed dead-on)
// can false-positive as "2D". Only ever called for n_points == 4; a
// triangle can't be a rectangle, so those are never considered 2D.
bool psxgpu_device::gpu_detect_2d_polygon( const PAIR *n_coord, const osd::gpu_vertex *v, int n_points )
{
	if( n_points != 4 )
		return false;

	int x0 = S11_COORD_X( n_coord[ 0 ] ), y0 = S11_COORD_Y( n_coord[ 0 ] );
	int x1 = S11_COORD_X( n_coord[ 1 ] ), y1 = S11_COORD_Y( n_coord[ 1 ] );
	int x2 = S11_COORD_X( n_coord[ 2 ] ), y2 = S11_COORD_Y( n_coord[ 2 ] );
	int x3 = S11_COORD_X( n_coord[ 3 ] ), y3 = S11_COORD_Y( n_coord[ 3 ] );

	if( y0 != y1 || y2 != y3 || x0 != x2 || x1 != x3 )
		return false;

	int screen_w = std::abs( x1 - x0 );
	int screen_h = std::abs( y2 - y0 );
	int uv_w = (int)std::lround( std::fabs( v[ 1 ].u - v[ 0 ].u ) );
	int uv_h = (int)std::lround( std::fabs( v[ 2 ].v - v[ 0 ].v ) );

	return screen_w == uv_w && screen_h == uv_h;
}

// See gpu_vertex::u_min/v_min/u_max/v_max in gpurender.h - stamps this
// primitive's own UV bounding box onto every one of its vertices, so a
// filtered sample (see submit_triangle(s)'s filterable parameter) can't
// bleed past this polygon's own texture footprint into whatever's packed
// next to it in the same shared VRAM texture page.
// PS1 samples a primitive's interpolated U/V at the *top-left corner* of each
// pixel; a modern GPU (and this target, at any scale) samples at the fragment
// centre. For an unflipped sprite that difference stays inside one texel, but
// wherever U or V *decreases* across the primitive (a flipped 2D sprite drawn
// as a polygon) nearest sampling lands one texel early and can pull in a
// neighbouring image. Ported from Beetle PSX HW's
// Calc_UVOffsets_Adjust_Verts() (mednafen/psx/gpu_polygon_sub.c, itself from
// parallel-psx): if U (or V) is decreasing along one screen axis and constant
// along the other, add 1 to that coordinate for the whole primitive. The
// Wild Arms 2 forest-sprite special case in the original is game-specific
// and not ported. Also reports whether the primitive "may be 2D" (some UV
// derivative is exactly zero), which gpu_set_polygon_uv_clamp() uses to trim
// the filter clamp by one texel. A quad is two triangles, (0,1,2) and
// (1,2,3), whose results accumulate into one shared offset.
void psxgpu_device::gpu_apply_uv_offsets( const PAIR *n_coord, osd::gpu_vertex *v, int n_points, bool &may_be_2d )
{
	int off_u = 0, off_v = 0;
	may_be_2d = false;

	auto tri = [ & ]( int a, int b, int c )
	{
		int64_t x[ 3 ] = { S11_COORD_X( n_coord[ a ] ), S11_COORD_X( n_coord[ b ] ), S11_COORD_X( n_coord[ c ] ) };
		int64_t y[ 3 ] = { S11_COORD_Y( n_coord[ a ] ), S11_COORD_Y( n_coord[ b ] ), S11_COORD_Y( n_coord[ c ] ) };
		int64_t u[ 3 ] = { (int64_t)v[ a ].u, (int64_t)v[ b ].u, (int64_t)v[ c ].u };
		int64_t w[ 3 ] = { (int64_t)v[ a ].v, (int64_t)v[ b ].v, (int64_t)v[ c ].v };

		int64_t abx = x[ 1 ] - x[ 0 ], aby = y[ 1 ] - y[ 0 ];
		int64_t bcx = x[ 2 ] - x[ 1 ], bcy = y[ 2 ] - y[ 1 ];
		int64_t cax = x[ 0 ] - x[ 2 ], cay = y[ 0 ] - y[ 2 ];

		int64_t dudx = -aby * u[ 2 ] - bcy * u[ 0 ] - cay * u[ 1 ];
		int64_t dvdx = -aby * w[ 2 ] - bcy * w[ 0 ] - cay * w[ 1 ];
		int64_t dudy = +abx * u[ 2 ] + bcx * u[ 0 ] + cax * u[ 1 ];
		int64_t dvdy = +abx * w[ 2 ] + bcx * w[ 0 ] + cax * w[ 1 ];
		int64_t area = bcx * cay - bcy * cax;
		int64_t tex_area = ( u[ 1 ] - u[ 0 ] ) * ( w[ 2 ] - w[ 0 ] ) - ( u[ 2 ] - u[ 0 ] ) * ( w[ 1 ] - w[ 0 ] );

		// PGXP depth differing across the vertices means real 3D geometry
		// that merely projected into this shape - leave it alone.
		bool is_3d = ( v[ a ].w != v[ b ].w ) || ( v[ b ].w != v[ c ].w );

		if( area == 0 || tex_area == 0 || is_3d )
			return;

		bool neg_area = area < 0;
		bool neg_dudx = ( dudx < 0 ) != neg_area;
		bool neg_dudy = ( dudy < 0 ) != neg_area;
		bool neg_dvdx = ( dvdx < 0 ) != neg_area;
		bool neg_dvdy = ( dvdy < 0 ) != neg_area;
		bool zero_dudx = dudx == 0, zero_dudy = dudy == 0;
		bool zero_dvdx = dvdx == 0, zero_dvdy = dvdy == 0;

		may_be_2d = may_be_2d || zero_dudy || zero_dudx || zero_dvdy || zero_dvdx;

		if( ( neg_dudx && zero_dudy ) || ( neg_dudy && zero_dudx ) )
			off_u = 1;
		if( ( neg_dvdx && zero_dvdy ) || ( neg_dvdy && zero_dvdx ) )
			off_v = 1;
	};

	tri( 0, 1, 2 );
	if( n_points == 4 )
		tri( 1, 2, 3 );

	if( off_u || off_v )
	{
		for( int i = 0; i < n_points; i++ )
		{
			v[ i ].u += (float)off_u;
			v[ i ].v += (float)off_v;
		}
	}
}

// Beetle's Extend_/Finalise_UVLimits(): the primitive's UV footprint (already
// including the offset above). A likely-2D primitive's max is trimmed by one
// (when it has any extent) - "in nearest neighbour we'll get very close to
// this UV but not close enough to actually sample it" - so a filter tap never
// reaches the texel just past the sprite's far edge. With a texture window
// active the footprint is meaningless (coordinates wrap), so don't clamp.
void psxgpu_device::gpu_set_polygon_uv_clamp( osd::gpu_vertex *v, int n_points, bool may_be_2d, bool window_active )
{
	float u_min = v[ 0 ].u, u_max = v[ 0 ].u;
	float v_min = v[ 0 ].v, v_max = v[ 0 ].v;
	for( int i = 1; i < n_points; i++ )
	{
		u_min = std::min( u_min, v[ i ].u );
		u_max = std::max( u_max, v[ i ].u );
		v_min = std::min( v_min, v[ i ].v );
		v_max = std::max( v_max, v[ i ].v );
	}
	if( window_active )
	{
		u_min = 0.0f; v_min = 0.0f; u_max = 255.0f; v_max = 255.0f;
	}
	else if( may_be_2d )
	{
		if( u_max > u_min ) u_max -= 1.0f;
		if( v_max > v_min ) v_max -= 1.0f;
	}
	for( int i = 0; i < n_points; i++ )
	{
		v[ i ].u_min = u_min; v[ i ].u_max = u_max;
		v[ i ].v_min = v_min; v[ i ].v_max = v_max;
	}
}

bool psxgpu_device::gpu_submit_flat_textured_polygon( int n_points )
{
	uint8_t n_cmd = BGR_C( m_packet.FlatTexturedPolygon.n_bgr );

	uint32_t n_clutx = ( m_packet.FlatTexturedPolygon.vertex[ 0 ].n_texture.w.h & 0x3f ) << 4;
	uint32_t n_cluty = ( m_packet.FlatTexturedPolygon.vertex[ 0 ].n_texture.w.h >> 6 ) & 0x3ff;

	decode_tpage( m_packet.FlatTexturedPolygon.vertex[ 1 ].n_texture.w.h );
	TEXTURESETUP

	gpu_maybe_set_texture_page( n_tx, n_ty, n_tp, n_clutx, n_cluty );

	float r = ( n_cmd & 0x01 ) ? ( 128.0f / 255.0f ) : ( BGR_R( m_packet.FlatTexturedPolygon.n_bgr ) / 255.0f );
	float g = ( n_cmd & 0x01 ) ? ( 128.0f / 255.0f ) : ( BGR_G( m_packet.FlatTexturedPolygon.n_bgr ) / 255.0f );
	float b = ( n_cmd & 0x01 ) ? ( 128.0f / 255.0f ) : ( BGR_B( m_packet.FlatTexturedPolygon.n_bgr ) / 255.0f );

	osd::gpu_vertex v[ 4 ];
	PAIR coords[ 4 ];
	int indices[ 4 ];
	for( int i = 0; i < n_points; i++ )
	{
		coords[ i ] = m_packet.FlatTexturedPolygon.vertex[ i ].n_coord;
		indices[ i ] = gpu_entry_index( m_packet.FlatTexturedPolygon.vertex[ i ].n_coord );
		v[ i ].r = r; v[ i ].g = g; v[ i ].b = b; v[ i ].a = 1.0f;
		v[ i ].u = (float)TEXTURE_U( m_packet.FlatTexturedPolygon.vertex[ i ].n_texture );
		v[ i ].v = (float)TEXTURE_V( m_packet.FlatTexturedPolygon.vertex[ i ].n_texture );
	}
	gpu_resolve_polygon_pgxp( indices, coords, n_points, v );
	bool may_be_2d;
	gpu_apply_uv_offsets( coords, v, n_points, may_be_2d );
	gpu_set_polygon_uv_clamp( v, n_points, may_be_2d, n_tww != 255 || n_twh != 255 );

	osd::gpu_blend_mode blend = gpu_blend_mode_for( n_cmd );
	bool filterable = !gpu_detect_2d_polygon( coords, v, n_points )
		|| gpu_filter_eligible( machine().osd().gpu_render_filter_exclude_2d_polygon(), blend );
	gpu_submit_triangle_pair( v[ 0 ], v[ 1 ], v[ 2 ], n_points == 4 ? v[ 3 ] : v[ 2 ], n_points, true, blend, filterable, gpu_polygon_cull_mask( coords, n_points ) );
	return true;
}

bool psxgpu_device::gpu_submit_gouraud_textured_polygon( int n_points )
{
	uint8_t n_cmd = BGR_C( m_packet.GouraudTexturedPolygon.vertex[ 0 ].n_bgr );

	uint32_t n_clutx = ( m_packet.GouraudTexturedPolygon.vertex[ 0 ].n_texture.w.h & 0x3f ) << 4;
	uint32_t n_cluty = ( m_packet.GouraudTexturedPolygon.vertex[ 0 ].n_texture.w.h >> 6 ) & 0x3ff;

	decode_tpage( m_packet.GouraudTexturedPolygon.vertex[ 1 ].n_texture.w.h );
	TEXTURESETUP

	gpu_maybe_set_texture_page( n_tx, n_ty, n_tp, n_clutx, n_cluty );

	osd::gpu_vertex v[ 4 ];
	PAIR coords[ 4 ];
	int indices[ 4 ];
	for( int i = 0; i < n_points; i++ )
	{
		coords[ i ] = m_packet.GouraudTexturedPolygon.vertex[ i ].n_coord;
		indices[ i ] = gpu_entry_index( m_packet.GouraudTexturedPolygon.vertex[ i ].n_coord );
		v[ i ].r = BGR_R( m_packet.GouraudTexturedPolygon.vertex[ i ].n_bgr ) / 255.0f;
		v[ i ].g = BGR_G( m_packet.GouraudTexturedPolygon.vertex[ i ].n_bgr ) / 255.0f;
		v[ i ].b = BGR_B( m_packet.GouraudTexturedPolygon.vertex[ i ].n_bgr ) / 255.0f;
		v[ i ].a = 1.0f;
		v[ i ].u = (float)TEXTURE_U( m_packet.GouraudTexturedPolygon.vertex[ i ].n_texture );
		v[ i ].v = (float)TEXTURE_V( m_packet.GouraudTexturedPolygon.vertex[ i ].n_texture );
	}
	gpu_resolve_polygon_pgxp( indices, coords, n_points, v );
	bool may_be_2d;
	gpu_apply_uv_offsets( coords, v, n_points, may_be_2d );
	gpu_set_polygon_uv_clamp( v, n_points, may_be_2d, n_tww != 255 || n_twh != 255 );

	osd::gpu_blend_mode blend = gpu_blend_mode_for( n_cmd );
	bool filterable = !gpu_detect_2d_polygon( coords, v, n_points )
		|| gpu_filter_eligible( machine().osd().gpu_render_filter_exclude_2d_polygon(), blend );
	gpu_submit_triangle_pair( v[ 0 ], v[ 1 ], v[ 2 ], n_points == 4 ? v[ 3 ] : v[ 2 ], n_points, true, blend, filterable, gpu_polygon_cull_mask( coords, n_points ) );
	return true;
}

// Rectangle/sprite primitives (FlatRectangle[8x8/16x16], FlatTexturedRectangle,
// Sprite8x8/16x16) were routed only to the software p_vram path, never to the
// GPU target - unlike the four polygon primitives above. Games that use a
// solid FlatRectangle to erase/clear regions of the screen each frame (a
// common PS1 technique, distinct from a full VRAM clear) would have that
// erase applied to the software VRAM but never reflected in the GPU target's
// persistent framebuffer (see the resize_target()-only-clears comment on
// gpu_update_screen() - VRAM is deliberately persistent across frames), so
// stale GPU-rendered pixels from prior frames stuck around as ghosting until
// something else happened to overdraw that exact region. Confirmed via
// gdarius2 (Sony ZN) live testing, 2026-09-17 - not visible on brvblade,
// which apparently doesn't rely on this technique. Fixed by giving rectangle/
// sprite primitives the same GPU path as the polygon primitives, as two
// triangles forming the axis-aligned quad.
bool psxgpu_device::gpu_submit_flat_rectangle( int32_t n_x, int32_t n_y, int32_t n_w, int32_t n_h, PAIR n_bgr )
{
	uint8_t n_cmd = BGR_C( n_bgr );
	float r = BGR_R( n_bgr ) / 255.0f;
	float g = BGR_G( n_bgr ) / 255.0f;
	float b = BGR_B( n_bgr ) / 255.0f;

	float x0 = (float)( n_x + n_drawoffset_x ) * gpu_scale();
	float y0 = (float)( n_y + n_drawoffset_y ) * gpu_scale();
	float x1 = x0 + (float)n_w * gpu_scale();
	float y1 = y0 + (float)n_h * gpu_scale();

	osd::gpu_vertex v[ 4 ];
	v[ 0 ].x = x0; v[ 0 ].y = y0;
	v[ 1 ].x = x1; v[ 1 ].y = y0;
	v[ 2 ].x = x0; v[ 2 ].y = y1;
	v[ 3 ].x = x1; v[ 3 ].y = y1;
	for( int i = 0; i < 4; i++ )
	{
		v[ i ].r = r; v[ i ].g = g; v[ i ].b = b; v[ i ].a = 1.0f;
		v[ i ].u = 0.0f; v[ i ].v = 0.0f;
	}

	gpu_submit_triangle_pair( v[ 0 ], v[ 1 ], v[ 2 ], v[ 3 ], 4, false, gpu_blend_mode_for( n_cmd ) );
	return true;
}

bool psxgpu_device::gpu_submit_textured_rectangle( int32_t n_x, int32_t n_y, int32_t n_w, int32_t n_h, uint8_t n_u0, uint8_t n_v0, PAIR n_bgr, int32_t n_tx, int32_t n_ty, int32_t n_tp, uint32_t n_clutx, uint32_t n_cluty )
{
	uint8_t n_cmd = BGR_C( n_bgr );

	gpu_maybe_set_texture_page( n_tx, n_ty, n_tp, n_clutx, n_cluty );

	float r = ( n_cmd & 0x01 ) ? ( 128.0f / 255.0f ) : ( BGR_R( n_bgr ) / 255.0f );
	float g = ( n_cmd & 0x01 ) ? ( 128.0f / 255.0f ) : ( BGR_G( n_bgr ) / 255.0f );
	float b = ( n_cmd & 0x01 ) ? ( 128.0f / 255.0f ) : ( BGR_B( n_bgr ) / 255.0f );

	// n_du/n_dv (sprite flip along x/y, from the current draw mode's texpage
	// bits) apply identically here to how the software TEXTUREFILL loop uses
	// them - a linear u0->u1/v0->v1 interpolation across the quad reproduces
	// the same flip, since PS1 rectangle/sprite texturing has no perspective
	// term to preserve either way.
	int n_du = ( n_ix != 0 ) ? -1 : 1;
	int n_dv = ( n_iy != 0 ) ? -1 : 1;

	float x0 = (float)( n_x + n_drawoffset_x ) * gpu_scale();
	float y0 = (float)( n_y + n_drawoffset_y ) * gpu_scale();
	float x1 = x0 + (float)n_w * gpu_scale();
	float y1 = y0 + (float)n_h * gpu_scale();

	float u0 = (float)n_u0;
	float v0 = (float)n_v0;
	float u1 = u0 + (float)( n_w * n_du );
	float v1 = v0 + (float)( n_h * n_dv );

	osd::gpu_vertex v[ 4 ];
	v[ 0 ].x = x0; v[ 0 ].y = y0; v[ 0 ].u = u0; v[ 0 ].v = v0;
	v[ 1 ].x = x1; v[ 1 ].y = y0; v[ 1 ].u = u1; v[ 1 ].v = v0;
	v[ 2 ].x = x0; v[ 2 ].y = y1; v[ 2 ].u = u0; v[ 2 ].v = v1;
	v[ 3 ].x = x1; v[ 3 ].y = y1; v[ 3 ].u = u1; v[ 3 ].v = v1;
	for( int i = 0; i < 4; i++ )
	{
		v[ i ].r = r; v[ i ].g = g; v[ i ].b = b; v[ i ].a = 1.0f;
	}

	// Sprites/2D-rectangle draws (also covers dots, which delegate here as
	// a 1x1 sprite) are pixel-art-style content - PS1 games routinely use
	// these for HUD/text/UI - so filtering is excluded here by default
	// (mame_psx_gpu_filter_exclude_sprite core option, see
	// gpu_filter_eligible()'s doc comment); user-configurable per-game in
	// case a specific game wants its sprites smoothed too.
	osd::gpu_blend_mode blend = gpu_blend_mode_for( n_cmd );
	bool filterable = gpu_filter_eligible( machine().osd().gpu_render_filter_exclude_sprite(), blend );
	gpu_submit_triangle_pair( v[ 0 ], v[ 1 ], v[ 2 ], v[ 3 ], 4, true, blend, filterable );
	return true;
}

// GP0 0x02 "Fill Rectangle in VRAM" - unlike FlatRectangle (0x60-63), this
// command operates on absolute VRAM addresses (no draw offset) and
// deliberately bypasses the draw-area clip on real hardware, which is why
// it's a common choice for the double-buffer clear PS1 games do each frame -
// matches the software path (FrameBufferRectangleDraw()) not applying
// n_drawoffset_x/y or checking n_drawarea_x1/y1/x2/y2 either. Since this
// target's local coordinate space already equals VRAM-absolute address
// space (every offset-relative primitive above ends up at coord +
// n_drawoffset == the VRAM address it's specified relative to), the
// absolute n_x/n_y here need no translation at all.
bool psxgpu_device::gpu_submit_vram_fill_rectangle( int32_t n_x, int32_t n_y, int32_t n_w, int32_t n_h, PAIR n_bgr )
{
	float r = BGR_R( n_bgr ) / 255.0f;
	float g = BGR_G( n_bgr ) / 255.0f;
	float b = BGR_B( n_bgr ) / 255.0f;

	float x0 = (float)n_x * gpu_scale();
	float y0 = (float)n_y * gpu_scale();
	float x1 = x0 + (float)n_w * gpu_scale();
	float y1 = y0 + (float)n_h * gpu_scale();

	osd::gpu_vertex v[ 4 ];
	v[ 0 ].x = x0; v[ 0 ].y = y0;
	v[ 1 ].x = x1; v[ 1 ].y = y0;
	v[ 2 ].x = x0; v[ 2 ].y = y1;
	v[ 3 ].x = x1; v[ 3 ].y = y1;
	for( int i = 0; i < 4; i++ )
	{
		v[ i ].r = r; v[ i ].g = g; v[ i ].b = b; v[ i ].a = 1.0f;
		v[ i ].u = 0.0f; v[ i ].v = 0.0f;
	}

	gpu_force_no_clip();
	gpu_queue_triangle_pair( v[ 0 ], v[ 1 ], v[ 2 ], v[ 3 ], 4, false, osd::gpu_blend_mode::NONE );
	return true;
}

// GP0 0xA0 "Copy Rectangle (CPU to VRAM)" - the standard way PS1 games DMA
// pre-rendered 2D art (logos, backgrounds, UI) straight into VRAM, entirely
// bypassing the polygon/sprite pipeline. Like the fill above, this is an
// absolute-VRAM-address, no-clip operation on real hardware. The pixel data
// itself still gets written to the software p_vram buffer exactly as before
// (see the inline 0xA0 handler in gpu_write()) - that write is what feeds
// this frame's upload_vram() call, so by the time gpu_update_screen()
// replays this queued command, the just-uploaded whole-VRAM texture already
// contains this transfer's data at address (n_x,n_y). This stamps it into
// the target by sampling that texture directly (texture page 2 = 16bpp
// direct color, no CLUT, tx=ty=0 since VRAM texture addressing is already
// absolute) rather than re-uploading the same bytes as a second texture.
bool psxgpu_device::gpu_submit_image_stamp( int32_t n_x, int32_t n_y, int32_t n_w, int32_t n_h )
{
	gpu_maybe_set_texture_page( 0, 0, 2, 0, 0, false );

	float x0 = (float)n_x * gpu_scale();
	float y0 = (float)n_y * gpu_scale();
	float x1 = x0 + (float)n_w * gpu_scale();
	float y1 = y0 + (float)n_h * gpu_scale();

	float u0 = (float)n_x;
	float v0 = (float)n_y;
	float u1 = (float)( n_x + n_w );
	float v1 = (float)( n_y + n_h );

	osd::gpu_vertex v[ 4 ];
	v[ 0 ].x = x0; v[ 0 ].y = y0; v[ 0 ].u = u0; v[ 0 ].v = v0;
	v[ 1 ].x = x1; v[ 1 ].y = y0; v[ 1 ].u = u1; v[ 1 ].v = v0;
	v[ 2 ].x = x0; v[ 2 ].y = y1; v[ 2 ].u = u0; v[ 2 ].v = v1;
	v[ 3 ].x = x1; v[ 3 ].y = y1; v[ 3 ].u = u1; v[ 3 ].v = v1;
	for( int i = 0; i < 4; i++ )
	{
		// neutral shade (matches the "raw" 0.5*2.0 = 1.0 modulation the
		// fragment shader expects - see its header comment).
		v[ i ].r = 0.5f; v[ i ].g = 0.5f; v[ i ].b = 0.5f; v[ i ].a = 1.0f;
	}

	gpu_force_no_clip();
	// filterable=false: this is a verbatim CPU-to-VRAM pixel copy (see the
	// comment above gpu_submit_image_stamp()), not a game asset meant to be
	// viewed smoothed/scaled - filtering it would blur data that's often
	// not even an image (e.g. a CLUT table stashed in VRAM).
	gpu_queue_triangle_pair( v[ 0 ], v[ 1 ], v[ 2 ], v[ 3 ], 4, true, osd::gpu_blend_mode::NONE, false );
	return true;
}

// GP0 0x80 "Move Image in Frame Buffer" (VRAM-to-VRAM copy) - also an
// absolute-VRAM-address, no-clip operation (matches MoveImage() not
// checking n_drawarea_*/applying n_drawoffset either). Unlike the image
// stamp above, the source data for this one may well be GPU-rendered
// content (a polygon/rectangle/sprite drawn earlier this frame or a prior
// one) that was never written back to the CPU-side p_vram buffer, so
// sampling the raw VRAM texture would be wrong - queued as a real
// GPU-side framebuffer-to-itself blit (osd::gpu_render_target::copy_rect())
// instead, replayed at the correct point in this frame's command order.
bool psxgpu_device::gpu_queue_copy_rect( int32_t n_sx, int32_t n_sy, int32_t n_dx, int32_t n_dy, int32_t n_w, int32_t n_h )
{
	gpu_queued_cmd cmd;
	cmd.kind = gpu_queued_cmd::kind_t::COPY;
	cmd.copy_sx = n_sx * gpu_scale();
	cmd.copy_sy = n_sy * gpu_scale();
	cmd.copy_dx = n_dx * gpu_scale();
	cmd.copy_dy = n_dy * gpu_scale();
	cmd.copy_w = n_w * gpu_scale();
	cmd.copy_h = n_h * gpu_scale();
	m_gpu_queue.push_back( std::move( cmd ) );
	return true;
}

// Dot/TexturedDot and Monochrome/GouraudLine, unlike the VRAM-absolute
// commands above, are offset-relative and draw-area-clipped exactly like
// the polygon primitives (see Dot()'s/MonochromeLine()'s own drawarea
// check in the software path), so these go through the normal
// gpu_submit_triangle_pair() path rather than gpu_force_no_clip().
bool psxgpu_device::gpu_submit_dot( int32_t n_x, int32_t n_y, PAIR n_bgr )
{
	float r = BGR_R( n_bgr ) / 255.0f;
	float g = BGR_G( n_bgr ) / 255.0f;
	float b = BGR_B( n_bgr ) / 255.0f;

	float x0 = (float)( n_x + n_drawoffset_x ) * gpu_scale();
	float y0 = (float)( n_y + n_drawoffset_y ) * gpu_scale();
	float x1 = x0 + gpu_scale();
	float y1 = y0 + gpu_scale();

	osd::gpu_vertex v[ 4 ];
	v[ 0 ].x = x0; v[ 0 ].y = y0;
	v[ 1 ].x = x1; v[ 1 ].y = y0;
	v[ 2 ].x = x0; v[ 2 ].y = y1;
	v[ 3 ].x = x1; v[ 3 ].y = y1;
	for( int i = 0; i < 4; i++ )
	{
		v[ i ].r = r; v[ i ].g = g; v[ i ].b = b; v[ i ].a = 1.0f;
		v[ i ].u = 0.0f; v[ i ].v = 0.0f;
	}

	gpu_submit_triangle_pair( v[ 0 ], v[ 1 ], v[ 2 ], v[ 3 ], 4, false, osd::gpu_blend_mode::NONE );
	return true;
}

bool psxgpu_device::gpu_submit_textured_dot( int32_t n_x, int32_t n_y, uint8_t n_u0, uint8_t n_v0, PAIR n_bgr, int32_t n_tx, int32_t n_ty, int32_t n_tp, uint32_t n_clutx, uint32_t n_cluty )
{
	return gpu_submit_textured_rectangle( n_x, n_y, 1, 1, n_u0, n_v0, n_bgr, n_tx, n_ty, n_tp, n_clutx, n_cluty );
}

bool psxgpu_device::gpu_submit_line( int32_t n_x0, int32_t n_y0, int32_t n_x1, int32_t n_y1, float r0, float g0, float b0, float r1, float g1, float b1, uint8_t n_cmd )
{
	float x0 = (float)( n_x0 + n_drawoffset_x ) * gpu_scale();
	float y0 = (float)( n_y0 + n_drawoffset_y ) * gpu_scale();
	float x1 = (float)( n_x1 + n_drawoffset_x ) * gpu_scale();
	float y1 = (float)( n_y1 + n_drawoffset_y ) * gpu_scale();

	// PS1 lines are always exactly 1 pixel wide with no anti-aliasing -
	// reproduced as a thin quad, half a (scaled) pixel to either side of
	// the segment, rather than adding a new GL_LINES draw path.
	float dx = x1 - x0;
	float dy = y1 - y0;
	float len = sqrtf( dx * dx + dy * dy );
	float nx, ny;
	if( len < 0.0001f )
	{
		nx = 0.5f * gpu_scale();
		ny = 0.0f;
	}
	else
	{
		nx = -dy / len * 0.5f * gpu_scale();
		ny = dx / len * 0.5f * gpu_scale();
	}

	osd::gpu_vertex v[ 4 ];
	v[ 0 ].x = x0 - nx; v[ 0 ].y = y0 - ny; v[ 0 ].r = r0; v[ 0 ].g = g0; v[ 0 ].b = b0;
	v[ 1 ].x = x0 + nx; v[ 1 ].y = y0 + ny; v[ 1 ].r = r0; v[ 1 ].g = g0; v[ 1 ].b = b0;
	v[ 2 ].x = x1 - nx; v[ 2 ].y = y1 - ny; v[ 2 ].r = r1; v[ 2 ].g = g1; v[ 2 ].b = b1;
	v[ 3 ].x = x1 + nx; v[ 3 ].y = y1 + ny; v[ 3 ].r = r1; v[ 3 ].g = g1; v[ 3 ].b = b1;
	for( int i = 0; i < 4; i++ )
	{
		v[ i ].a = 1.0f; v[ i ].u = 0.0f; v[ i ].v = 0.0f;
	}

	gpu_submit_triangle_pair( v[ 0 ], v[ 1 ], v[ 2 ], v[ 3 ], 4, false, gpu_blend_mode_for( n_cmd ) );
	return true;
}

// Re-issuing a set_texture_page() GL state change on every single textured
// polygon was wasteful - most runs of consecutive polygons reuse the same
// texture page/CLUT, so skip it entirely when nothing has changed since
// last time. Unlike the old per-page decode this replaced, there is no
// expensive CPU work being skipped here (just a handful of glUniform1i
// calls) - this cache is now purely about avoiding a GL state churn, not
// about avoiding a decode.
void psxgpu_device::gpu_maybe_set_texture_page( int n_tx, int n_ty, int tp, int n_clutx, int n_cluty, bool use_window )
{
	// Texture window (GP0 E2) rides along with every textured draw's
	// texture-page setup, cached the same way. The software path's
	// TEXTURESETUP/TEXTUREWINDOW* macros apply (u & n_tww) + n_twx and
	// (v & n_twh) + n_twy - see gpurender.h's set_texture_window().
	//
	// use_window = false forces the identity window: the VRAM image stamp
	// (gpu_submit_image_stamp()) isn't a real texture draw - it samples
	// absolute VRAM coordinates (up to 1023/511) - so the game's own
	// texture window must not remap them. Found via cryptklr (Konami GQ),
	// whose story-screen image uploads were shredded into stripes by an
	// active window; the next real textured draw re-emits the game's window
	// because the cache compares against what was actually last sent.
	int32_t w_and_u = use_window ? (int32_t)n_tww : 255;
	int32_t w_and_v = use_window ? (int32_t)n_twh : 255;
	int32_t w_off_u = use_window ? (int32_t)n_twx : 0;
	int32_t w_off_v = use_window ? (int32_t)n_twy : 0;
	// Interleaved texture pages (type-1 GPU tpage bit 13, n_ti) - see
	// gpurender.h's set_texture_interleave(). Only meaningful for real
	// texture draws; the image stamp always samples plain VRAM.
	int32_t w_ti = ( use_window && n_ti != 0 ) ? 1 : 0;
	if( w_and_u != m_gpu_last_tw_and_u || w_and_v != m_gpu_last_tw_and_v ||
		w_off_u != m_gpu_last_tw_off_u || w_off_v != m_gpu_last_tw_off_v ||
		w_ti != m_gpu_last_interleave )
	{
		gpu_queued_cmd wcmd;
		wcmd.kind = gpu_queued_cmd::kind_t::TEXWIN;
		wcmd.tw_and_u = w_and_u;
		wcmd.tw_and_v = w_and_v;
		wcmd.tw_off_u = w_off_u;
		wcmd.tw_off_v = w_off_v;
		wcmd.tw_interleave = w_ti;
		m_gpu_queue.push_back( std::move( wcmd ) );
		m_gpu_last_tw_and_u = w_and_u;
		m_gpu_last_tw_and_v = w_and_v;
		m_gpu_last_tw_off_u = w_off_u;
		m_gpu_last_tw_off_v = w_off_v;
		m_gpu_last_interleave = w_ti;
	}

	if( n_tx == m_gpu_last_tx && n_ty == m_gpu_last_ty && tp == m_gpu_last_tp &&
		n_clutx == m_gpu_last_clutx && n_cluty == m_gpu_last_cluty )
	{
		return;
	}

	gpu_queued_cmd cmd;
	cmd.kind = gpu_queued_cmd::kind_t::TEXPARAM;
	cmd.tex_tx = n_tx;
	cmd.tex_ty = n_ty;
	cmd.tex_tp = tp;
	cmd.tex_clutx = n_clutx;
	cmd.tex_cluty = n_cluty;
	m_gpu_queue.push_back( std::move( cmd ) );

	m_gpu_last_tx = n_tx;
	m_gpu_last_ty = n_ty;
	m_gpu_last_tp = tp;
	m_gpu_last_clutx = n_clutx;
	m_gpu_last_cluty = n_cluty;
}

// Replays this frame's queued set_texture_page()/set_clip_rect()/triangle
// commands (see the m_gpu_queue comment in psx.h), reads the result back
// into bitmap, then clears the queue for the next frame. Consecutive
// TRIANGLE commands sharing the same textured/blend/filterable state are
// merged into one osd::gpu_render_target::submit_triangles() call instead
// of replayed one triangle (one GL draw call) at a time - a run only needs
// to break on an actual state change (different textured/blend/filterable)
// or an intervening
// TEXTURE/CLIP command (which changes what subsequent triangles should
// look like, so can't be reordered past). The GPU target itself is NOT
// cleared here (see retro_gpu_target::resize_target()) - it models PS1
// VRAM, which is persistent across frames, matching the original
// VRAM-scanout path. A game that doesn't fully redraw the visible area
// every single refresh (common - most games only resubmit what changed,
// clearing the rest via their own fill-rectangle GP0 command) still shows
// correctly-persisted content from prior frames instead of the
// un-resubmitted parts going black.
uint32_t psxgpu_device::gpu_update_screen( bitmap_rgb32 &bitmap )
{

	int w = n_screenwidth * gpu_scale();

	// The actual GPU target is sized to full VRAM height, not just the
	// currently-declared display height - draw-offset-relative geometry
	// (polygons, rectangles, etc.) is placed at coord + n_drawoffset, which
	// is a real VRAM address that can legitimately land well past
	// n_screenheight (e.g. a game double-buffering across a taller region
	// of VRAM than what's actually shown at once - draw into the hidden
	// half, flip display_start to reveal it, repeat). Sizing the target to
	// only n_screenheight silently clipped/discarded any draw-offset
	// commands aimed at such a hidden buffer - found via raystorm (Taito,
	// src/mame/sony/zn.cpp): n_screenheight=240 there, but it flips
	// n_drawoffset_y/n_displaystarty between 0 and 240 every frame,
	// meaning VRAM rows 0-479 are all live, not just 0-239.
	int h = m_vram_height * gpu_scale();

	// One context acquisition for the whole burst below (begin_frame,
	// every queued call, and the final readback) instead of one per call -
	// see the begin_batch()/end_batch() comment in retro_gpu_target.h for
	// why this specific usage (a tight synchronous loop with nothing else
	// running in between) is safe where holding the context across
	// multiple *separate* MAME calls was not.
	m_gpu_render_target->begin_batch();
	m_gpu_render_target->begin_frame( w, h );

	// Whole-VRAM upload, once per frame, regardless of how many texture
	// page/CLUT switches this frame's polygons make - see the fragment
	// shader comment in retro_gpu_target.cpp for why this replaced a
	// per-texture-page CPU decode (that was 43% of total CPU time in
	// profiling, more than PS1 CPU emulation itself).
	m_gpu_render_target->upload_vram( p_vram, 1024, m_vram_height );

	std::vector<osd::gpu_vertex> run;
	bool run_textured = false;
	bool run_filterable = true;
	osd::gpu_blend_mode run_blend = osd::gpu_blend_mode::NONE;
	auto flush_run = [ this, &run, &run_textured, &run_filterable, &run_blend ]()
	{
		if( !run.empty() )
		{
			m_gpu_render_target->submit_triangles( run.data(), (int)run.size(), run_textured, run_blend, run_filterable );
			run.clear();
		}
	};

	for( auto &cmd : m_gpu_queue )
	{
		switch( cmd.kind )
		{
		case gpu_queued_cmd::kind_t::TRIANGLE:
			if( !run.empty() && ( cmd.textured != run_textured || cmd.blend != run_blend || cmd.filterable != run_filterable ) )
				flush_run();
			run_textured = cmd.textured;
			run_filterable = cmd.filterable;
			run_blend = cmd.blend;
			run.push_back( cmd.tri[ 0 ] );
			run.push_back( cmd.tri[ 1 ] );
			run.push_back( cmd.tri[ 2 ] );
			break;
		case gpu_queued_cmd::kind_t::TEXPARAM:
			flush_run();
			m_gpu_render_target->set_texture_page( cmd.tex_tx, cmd.tex_ty, cmd.tex_tp, cmd.tex_clutx, cmd.tex_cluty );
			break;
		case gpu_queued_cmd::kind_t::TEXWIN:
			flush_run();
			m_gpu_render_target->set_texture_window( cmd.tw_and_u, cmd.tw_and_v, cmd.tw_off_u, cmd.tw_off_v );
			m_gpu_render_target->set_texture_interleave( cmd.tw_interleave != 0 );
			break;
		case gpu_queued_cmd::kind_t::CLIP:
			flush_run();
			m_gpu_render_target->set_clip_rect( cmd.clip_x1, cmd.clip_y1, cmd.clip_x2, cmd.clip_y2 );
			break;
		case gpu_queued_cmd::kind_t::COPY:
			flush_run();
			m_gpu_render_target->copy_rect( cmd.copy_sx, cmd.copy_sy, cmd.copy_dx, cmd.copy_dy, cmd.copy_w, cmd.copy_h );
			break;
		}
	}
	flush_run();
	m_gpu_queue.clear();

	std::vector<uint32_t> temp( (size_t)w * h );
	m_gpu_render_target->end_frame_and_readback( temp.data() );
	m_gpu_render_target->end_batch();

	// Mirrors update_screen()'s (the software display path, further down in
	// this file) border/overscan cropping and display_start windowing -
	// the GPU path previously showed the raw n_screenwidth x n_screenheight
	// canvas verbatim, unlike the software path, which only ever shows the
	// n_vert_disstart/n_vert_disend/n_horiz_disstart/n_horiz_disend window
	// and paints solid black everywhere else. Real PS1 games rely on that
	// crop: they routinely stash non-image scratch data (CLUT tables, work
	// buffers) in the overscan border, counting on real hardware never
	// scanning it out - found via raystorm (Taito, src/mame/sony/zn.cpp),
	// which stores a small CLUT swatch there, visibly rendered as a stray
	// block of colour in the corner without this crop. GP1 0x05's
	// "display start" is applied the same way as before (see the removed
	// version of this comment in git history for that half on its own),
	// just folded into the same window computation - both were found and
	// fixed together, 2026-09-17.
	if( ( n_gpustatus & ( 1 << 0x17 ) ) != 0 )
	{
		/* display disabled */
		bitmap.fill( 0 );
		return 0;
	}

	int scale = gpu_scale();
	int n_displaystartx;
	if( b_reverseflag )
	{
		n_displaystartx = ( 1023 - m_n_displaystartx );
		n_displaystartx -= ( (int32_t)n_screenwidth - 1 );
	}
	else
	{
		n_displaystartx = m_n_displaystartx;
	}

	int n_overscantop, n_overscanleft;
	if( ( n_gpustatus & ( 1 << 0x14 ) ) != 0 )
	{
		/* pal */
		n_overscantop = 0x23;
		n_overscanleft = 0x27e;
	}
	else
	{
		/* ntsc */
		n_overscantop = 0x10;
		n_overscanleft = 0x260;
	}

	int n_top = (int32_t)n_vert_disstart - n_overscantop;
	int n_lines = (int32_t)n_vert_disend - (int32_t)n_vert_disstart;
	int n_y;
	if( n_top < 0 )
	{
		n_y = -n_top;
		n_lines += n_top;
	}
	else
	{
		n_y = 0;
	}
	if( ( n_gpustatus & ( 1 << 0x16 ) ) != 0 )
	{
		/* interlaced */
		n_lines *= 2;
	}
	if( n_lines > (int32_t)n_screenheight - ( n_y + n_top ) )
		n_lines = (int32_t)n_screenheight - ( n_y + n_top );

	int n_left = ( ( (int32_t)n_horiz_disstart - n_overscanleft ) * (int32_t)n_screenwidth ) / 2560;
	int n_columns = ( ( (int32_t)n_horiz_disend - (int32_t)n_horiz_disstart ) * (int32_t)n_screenwidth ) / 2560;
	if( n_left > (int32_t)n_screenwidth - n_columns )
		n_left = (int32_t)n_screenwidth - n_columns;
	int n_x;
	if( n_left < 0 )
	{
		n_x = -n_left;
		n_columns += n_left;
	}
	else
	{
		n_x = 0;
	}
	if( n_columns > (int32_t)n_screenwidth - ( n_x + n_left ) )
		n_columns = (int32_t)n_screenwidth - ( n_x + n_left );

	bitmap.fill( 0 );

	// 24-bit display mode (GP1 08h bit 4 - what MDEC-decoded movies use):
	// VRAM holds packed 24-bit RGB, three 16-bit words per two pixels, so
	// the GPU render target's 15-bit colour is meaningless for that region
	// and showing it read as noise (Tekken Tag Tournament's intro movie).
	// p_p_vram is still the authoritative copy of anything written by the
	// CPU/DMA/MDEC (image transfers write it even in GPU mode - see
	// gpu_submit_image_stamp()), so decode straight from it exactly like
	// update_screen()'s 24-bit branch, then replicate each native pixel
	// scale x scale to fill the scaled bitmap.
	if( ( n_gpustatus & ( 1 << 0x15 ) ) != 0 )
	{
		if( n_lines > 0 && n_columns > 0 )
		{
			int dst_y0 = ( n_y + n_top ) * scale;
			int dst_x0 = ( n_x + n_left ) * scale;
			std::vector<uint32_t> native_row( (size_t)n_columns );
			for( int line = 0; line < n_lines; line++ )
			{
				const uint16_t *p_src = p_p_vram[ ( n_y + (int32_t)n_displaystarty + line ) & 1023 ];
				int word = 3 * n_x + n_displaystartx;
				int col = 0;
				while( col < n_columns )
				{
					uint32_t n_g0r0 = p_src[ ( word++ ) & 1023 ];
					uint32_t n_r1b0 = p_src[ ( word++ ) & 1023 ];
					uint32_t n_b1g1 = p_src[ ( word++ ) & 1023 ];
					native_row[ col++ ] = p_n_g0r0[ n_g0r0 ] | p_n_b0[ n_r1b0 ];
					if( col < n_columns )
						native_row[ col++ ] = p_n_r1[ n_r1b0 ] | p_n_b1g1[ n_b1g1 ];
				}
				for( int ry = 0; ry < scale; ry++ )
				{
					int dy = dst_y0 + line * scale + ry;
					if( dy < 0 || dy >= bitmap.height() )
						continue;
					uint32_t *dst_row = &bitmap.pix( dy );
					for( int c = 0; c < n_columns; c++ )
					{
						for( int rx = 0; rx < scale; rx++ )
						{
							int dx = dst_x0 + c * scale + rx;
							if( dx >= 0 && dx < bitmap.width() )
								dst_row[ dx ] = native_row[ c ];
						}
					}
				}
			}
		}
		return 0;
	}

	if( n_lines > 0 && n_columns > 0 )
	{
		int dst_y0 = ( n_y + n_top ) * scale;
		int dst_x0 = ( n_x + n_left ) * scale;
		int src_y0 = ( n_y + (int32_t)n_displaystarty ) * scale;
		int src_x0 = ( n_x + n_displaystartx ) * scale;
		int copy_h = std::min( n_lines * scale, bitmap.height() - dst_y0 );
		int copy_w = std::min( n_columns * scale, bitmap.width() - dst_x0 );

		if( copy_h > 0 && copy_w > 0 )
		{
			int sx = ( ( src_x0 % w ) + w ) % w;
			for( int y = 0; y < copy_h; y++ )
			{
				int src_y = ( ( ( src_y0 + y ) % h ) + h ) % h;
				const uint32_t *src_row = &temp[ (size_t)src_y * w ];
				uint32_t *dst_row = &bitmap.pix( dst_y0 + y, dst_x0 );
				if( sx == 0 )
				{
					memcpy( dst_row, src_row, (size_t)copy_w * sizeof( uint32_t ) );
				}
				else
				{
					int first_part = std::min( copy_w, w - sx );
					memcpy( dst_row, src_row + sx, (size_t)first_part * sizeof( uint32_t ) );
					if( copy_w > first_part )
						memcpy( dst_row + first_part, src_row, (size_t)( copy_w - first_part ) * sizeof( uint32_t ) );
				}
			}
		}
	}

	return 0;
}

void psxgpu_device::FlatPolygon( int n_points )
{
	if( gpu_active() )
	{
		gpu_submit_flat_polygon( n_points );
		return;
	}

#if PSXGPU_DEBUG_VIEWER
	if( m_debug.n_skip == 1 )
	{
		return;
	}
	for( int n_point = 0; n_point < n_points; n_point++ )
	{
		DebugMesh( S11_COORD_X( m_packet.FlatPolygon.vertex[ n_point ].n_coord ) + n_drawoffset_x, S11_COORD_Y( m_packet.FlatPolygon.vertex[ n_point ].n_coord ) + n_drawoffset_y );
	}
	DebugMeshEnd();
#endif

	uint8_t n_cmd = BGR_C( m_packet.FlatPolygon.n_bgr );

	PAIR n_cx1; n_cx1.d = 0;
	PAIR n_cx2; n_cx2.d = 0;

	SOLIDSETUP

	PAIR n_r; n_r.w.h = BGR_R( m_packet.FlatPolygon.n_bgr ); n_r.w.l = 0;
	PAIR n_g; n_g.w.h = BGR_G( m_packet.FlatPolygon.n_bgr ); n_g.w.l = 0;
	PAIR n_b; n_b.w.h = BGR_B( m_packet.FlatPolygon.n_bgr ); n_b.w.l = 0;

	FINDTOPLEFT( FlatPolygon )

	int32_t n_dx1 = 0;
	int32_t n_dx2 = 0;

	int16_t n_y = COORD_Y( m_packet.FlatPolygon.vertex[ n_rightpoint ].n_coord );

	for( ;; )
	{
		if( n_y == COORD_Y( m_packet.FlatPolygon.vertex[ n_leftpoint ].n_coord ) )
		{
			while( n_y == COORD_Y( m_packet.FlatPolygon.vertex[ p_n_leftpointlist[ n_leftpoint ] ].n_coord ) )
			{
				n_leftpoint = p_n_leftpointlist[ n_leftpoint ];
				if( n_leftpoint == n_rightpoint )
				{
					break;
				}
			}

			n_cx1.sw.h = COORD_X( m_packet.FlatPolygon.vertex[ n_leftpoint ].n_coord ); n_cx1.sw.l = 0;
			n_leftpoint = p_n_leftpointlist[ n_leftpoint ];

			int32_t n_distance = COORD_Y( m_packet.FlatPolygon.vertex[ n_leftpoint ].n_coord ) - n_y;
			if( n_distance < 1 )
			{
				break;
			}

			n_dx1 = (int32_t)( ( COORD_X( m_packet.FlatPolygon.vertex[ n_leftpoint ].n_coord ) << 16 ) - n_cx1.d ) / n_distance;
		}

		if( n_y == COORD_Y( m_packet.FlatPolygon.vertex[ n_rightpoint ].n_coord ) )
		{
			while( n_y == COORD_Y( m_packet.FlatPolygon.vertex[ p_n_rightpointlist[ n_rightpoint ] ].n_coord ) )
			{
				n_rightpoint = p_n_rightpointlist[ n_rightpoint ];
				if( n_rightpoint == n_leftpoint )
				{
					break;
				}
			}

			n_cx2.sw.h = COORD_X( m_packet.FlatPolygon.vertex[ n_rightpoint ].n_coord ); n_cx2.sw.l = 0;
			n_rightpoint = p_n_rightpointlist[ n_rightpoint ];

			int32_t n_distance = COORD_Y( m_packet.FlatPolygon.vertex[ n_rightpoint ].n_coord ) - n_y;
			if( n_distance < 1 )
			{
				break;
			}

			n_dx2 = (int32_t)( ( COORD_X( m_packet.FlatPolygon.vertex[ n_rightpoint ].n_coord ) << 16 ) - n_cx2.d ) / n_distance;
		}

		int drawy = n_y + n_drawoffset_y;

		if( (int16_t)n_cx1.sw.h != (int16_t)n_cx2.sw.h && drawy >= (int32_t)n_drawarea_y1 && drawy <= (int32_t)n_drawarea_y2 )
		{
			int16_t n_x;
			int32_t n_distance;

			if( (int16_t)n_cx1.sw.h < (int16_t)n_cx2.sw.h )
			{
				n_x = n_cx1.sw.h;
				n_distance = (int16_t)n_cx2.sw.h - n_x;
			}
			else
			{
				n_x = n_cx2.sw.h;
				n_distance = (int16_t)n_cx1.sw.h - n_x;
			}

			int drawx = n_x + n_drawoffset_x;

			if( ( (int32_t)n_drawarea_x1 - drawx ) > 0 )
			{
				n_distance -= ( n_drawarea_x1 - drawx );
				drawx = n_drawarea_x1;
			}

			SOLIDFILL( FLATPOLYGONUPDATE )
		}

		n_cx1.d += n_dx1;
		n_cx2.d += n_dx2;
		n_y++;
	}
}

void psxgpu_device::FlatTexturedPolygon( int n_points )
{
	if( gpu_active() )
	{
		gpu_submit_flat_textured_polygon( n_points );
		return;
	}

#if PSXGPU_DEBUG_VIEWER
	if( m_debug.n_skip == 2 )
	{
		return;
	}
	for( int n_point = 0; n_point < n_points; n_point++ )
	{
		DebugMesh( S11_COORD_X( m_packet.FlatTexturedPolygon.vertex[ n_point ].n_coord ) + n_drawoffset_x, S11_COORD_Y( m_packet.FlatTexturedPolygon.vertex[ n_point ].n_coord ) + n_drawoffset_y );
	}
	DebugMeshEnd();
#endif

	uint8_t n_cmd = BGR_C( m_packet.FlatTexturedPolygon.n_bgr );

	uint32_t n_clutx = ( m_packet.FlatTexturedPolygon.vertex[ 0 ].n_texture.w.h & 0x3f ) << 4;
	uint32_t n_cluty = ( m_packet.FlatTexturedPolygon.vertex[ 0 ].n_texture.w.h >> 6 ) & 0x3ff;

	PAIR n_cx1; n_cx1.d = 0;
	PAIR n_cu1; n_cu1.d = 0;
	PAIR n_cv1; n_cv1.d = 0;
	PAIR n_cx2; n_cx2.d = 0;
	PAIR n_cu2; n_cu2.d = 0;
	PAIR n_cv2; n_cv2.d = 0;

	decode_tpage( m_packet.FlatTexturedPolygon.vertex[ 1 ].n_texture.w.h );
	TEXTURESETUP

	PAIR n_r; n_r.w.h = n_cmd & 0x01 ? 0x80 : BGR_R( m_packet.FlatTexturedPolygon.n_bgr ); n_r.w.l = 0;
	PAIR n_g; n_g.w.h = n_cmd & 0x01 ? 0x80 : BGR_G( m_packet.FlatTexturedPolygon.n_bgr ); n_g.w.l = 0;
	PAIR n_b; n_b.w.h = n_cmd & 0x01 ? 0x80 : BGR_B( m_packet.FlatTexturedPolygon.n_bgr ); n_b.w.l = 0;

	FINDTOPLEFT( FlatTexturedPolygon )

	int32_t n_dx1 = 0;
	int32_t n_dx2 = 0;
	int32_t n_du1 = 0;
	int32_t n_du2 = 0;
	int32_t n_dv1 = 0;
	int32_t n_dv2 = 0;

	int16_t n_y = COORD_Y( m_packet.FlatTexturedPolygon.vertex[ n_rightpoint ].n_coord );

	for( ;; )
	{
		if( n_y == COORD_Y( m_packet.FlatTexturedPolygon.vertex[ n_leftpoint ].n_coord ) )
		{
			while( n_y == COORD_Y( m_packet.FlatTexturedPolygon.vertex[ p_n_leftpointlist[ n_leftpoint ] ].n_coord ) )
			{
				n_leftpoint = p_n_leftpointlist[ n_leftpoint ];
				if( n_leftpoint == n_rightpoint )
				{
					break;
				}
			}

			n_cx1.sw.h = COORD_X( m_packet.FlatTexturedPolygon.vertex[ n_leftpoint ].n_coord ); n_cx1.sw.l = 0;
			n_cu1.w.h = TEXTURE_U( m_packet.FlatTexturedPolygon.vertex[ n_leftpoint ].n_texture ); n_cu1.w.l = 0;
			n_cv1.w.h = TEXTURE_V( m_packet.FlatTexturedPolygon.vertex[ n_leftpoint ].n_texture ); n_cv1.w.l = 0;
			n_leftpoint = p_n_leftpointlist[ n_leftpoint ];

			int32_t n_distance = COORD_Y( m_packet.FlatTexturedPolygon.vertex[ n_leftpoint ].n_coord ) - n_y;
			if( n_distance < 1 )
			{
				break;
			}

			n_dx1 = (int32_t)( ( COORD_X( m_packet.FlatTexturedPolygon.vertex[ n_leftpoint ].n_coord ) << 16 ) - n_cx1.d ) / n_distance;
			n_du1 = (int32_t)( ( TEXTURE_U( m_packet.FlatTexturedPolygon.vertex[ n_leftpoint ].n_texture ) << 16 ) - n_cu1.d ) / n_distance;
			n_dv1 = (int32_t)( ( TEXTURE_V( m_packet.FlatTexturedPolygon.vertex[ n_leftpoint ].n_texture ) << 16 ) - n_cv1.d ) / n_distance;
		}

		if( n_y == COORD_Y( m_packet.FlatTexturedPolygon.vertex[ n_rightpoint ].n_coord ) )
		{
			while( n_y == COORD_Y( m_packet.FlatTexturedPolygon.vertex[ p_n_rightpointlist[ n_rightpoint ] ].n_coord ) )
			{
				n_rightpoint = p_n_rightpointlist[ n_rightpoint ];
				if( n_rightpoint == n_leftpoint )
				{
					break;
				}
			}

			n_cx2.sw.h = COORD_X( m_packet.FlatTexturedPolygon.vertex[ n_rightpoint ].n_coord ); n_cx2.sw.l = 0;
			n_cu2.w.h = TEXTURE_U( m_packet.FlatTexturedPolygon.vertex[ n_rightpoint ].n_texture ); n_cu2.w.l = 0;
			n_cv2.w.h = TEXTURE_V( m_packet.FlatTexturedPolygon.vertex[ n_rightpoint ].n_texture ); n_cv2.w.l = 0;
			n_rightpoint = p_n_rightpointlist[ n_rightpoint ];

			int32_t n_distance = COORD_Y( m_packet.FlatTexturedPolygon.vertex[ n_rightpoint ].n_coord ) - n_y;
			if( n_distance < 1 )
			{
				break;
			}

			n_dx2 = (int32_t)( ( COORD_X( m_packet.FlatTexturedPolygon.vertex[ n_rightpoint ].n_coord ) << 16 ) - n_cx2.d ) / n_distance;
			n_du2 = (int32_t)( ( TEXTURE_U( m_packet.FlatTexturedPolygon.vertex[ n_rightpoint ].n_texture ) << 16 ) - n_cu2.d ) / n_distance;
			n_dv2 = (int32_t)( ( TEXTURE_V( m_packet.FlatTexturedPolygon.vertex[ n_rightpoint ].n_texture ) << 16 ) - n_cv2.d ) / n_distance;
		}

		int drawy = n_y + n_drawoffset_y;

		if( (int16_t)n_cx1.sw.h != (int16_t)n_cx2.sw.h && drawy >= (int32_t)n_drawarea_y1 && drawy <= (int32_t)n_drawarea_y2 )
		{
			int16_t n_x;
			int32_t n_distance;
			PAIR n_u;
			PAIR n_v;
			int32_t n_du;
			int32_t n_dv;

			if( (int16_t)n_cx1.sw.h < (int16_t)n_cx2.sw.h )
			{
				n_x = n_cx1.sw.h;
				n_distance = (int16_t)n_cx2.sw.h - n_x;

				n_u.d = n_cu1.d;
				n_v.d = n_cv1.d;
				n_du = (int32_t)( n_cu2.d - n_cu1.d ) / n_distance;
				n_dv = (int32_t)( n_cv2.d - n_cv1.d ) / n_distance;
			}
			else
			{
				n_x = n_cx2.sw.h;
				n_distance = (int16_t)n_cx1.sw.h - n_x;

				n_u.d = n_cu2.d;
				n_v.d = n_cv2.d;
				n_du = (int32_t)( n_cu1.d - n_cu2.d ) / n_distance;
				n_dv = (int32_t)( n_cv1.d - n_cv2.d ) / n_distance;
			}

			int drawx = n_x + n_drawoffset_x;

			if( ( (int32_t)n_drawarea_x1 - drawx ) > 0 )
			{
				n_u.d += n_du * ( n_drawarea_x1 - drawx );
				n_v.d += n_dv * ( n_drawarea_x1 - drawx );
				n_distance -= ( n_drawarea_x1 - drawx );
				drawx = n_drawarea_x1;
			}

			TEXTUREFILL( FLATTEXTUREDPOLYGONUPDATE, n_u.w.h, n_v.w.h );
		}

		n_cx1.d += n_dx1;
		n_cu1.d += n_du1;
		n_cv1.d += n_dv1;
		n_cx2.d += n_dx2;
		n_cu2.d += n_du2;
		n_cv2.d += n_dv2;
		n_y++;
	}
}

void psxgpu_device::GouraudPolygon( int n_points )
{
	if( gpu_active() )
	{
		gpu_submit_gouraud_polygon( n_points );
		return;
	}

#if PSXGPU_DEBUG_VIEWER
	if( m_debug.n_skip == 3 )
	{
		return;
	}
	for( int n_point = 0; n_point < n_points; n_point++ )
	{
		DebugMesh( S11_COORD_X( m_packet.GouraudPolygon.vertex[ n_point ].n_coord ) + n_drawoffset_x, S11_COORD_Y( m_packet.GouraudPolygon.vertex[ n_point ].n_coord ) + n_drawoffset_y );
	}
	DebugMeshEnd();
#endif

	uint8_t n_cmd = BGR_C( m_packet.GouraudPolygon.vertex[ 0 ].n_bgr );

	PAIR n_cx1; n_cx1.d = 0;
	PAIR n_cr1; n_cr1.d = 0;
	PAIR n_cg1; n_cg1.d = 0;
	PAIR n_cb1; n_cb1.d = 0;
	PAIR n_cx2; n_cx2.d = 0;
	PAIR n_cr2; n_cr2.d = 0;
	PAIR n_cg2; n_cg2.d = 0;
	PAIR n_cb2; n_cb2.d = 0;

	SOLIDSETUP

	FINDTOPLEFT( GouraudPolygon )

	int32_t n_dx1 = 0;
	int32_t n_dx2 = 0;
	int32_t n_dr1 = 0;
	int32_t n_dr2 = 0;
	int32_t n_dg1 = 0;
	int32_t n_dg2 = 0;
	int32_t n_db1 = 0;
	int32_t n_db2 = 0;

	int16_t n_y = COORD_Y( m_packet.GouraudPolygon.vertex[ n_rightpoint ].n_coord );

	for( ;; )
	{
		if( n_y == COORD_Y( m_packet.GouraudPolygon.vertex[ n_leftpoint ].n_coord ) )
		{
			while( n_y == COORD_Y( m_packet.GouraudPolygon.vertex[ p_n_leftpointlist[ n_leftpoint ] ].n_coord ) )
			{
				n_leftpoint = p_n_leftpointlist[ n_leftpoint ];
				if( n_leftpoint == n_rightpoint )
				{
					break;
				}
			}

			n_cx1.sw.h = COORD_X( m_packet.GouraudPolygon.vertex[ n_leftpoint ].n_coord ); n_cx1.sw.l = 0;
			n_cr1.w.h = BGR_R( m_packet.GouraudPolygon.vertex[ n_leftpoint ].n_bgr ); n_cr1.w.l = 0;
			n_cg1.w.h = BGR_G( m_packet.GouraudPolygon.vertex[ n_leftpoint ].n_bgr ); n_cg1.w.l = 0;
			n_cb1.w.h = BGR_B( m_packet.GouraudPolygon.vertex[ n_leftpoint ].n_bgr ); n_cb1.w.l = 0;
			n_leftpoint = p_n_leftpointlist[ n_leftpoint ];

			int32_t n_distance = COORD_Y( m_packet.GouraudPolygon.vertex[ n_leftpoint ].n_coord ) - n_y;
			if( n_distance < 1 )
			{
				break;
			}

			n_dx1 = (int32_t)( ( COORD_X( m_packet.GouraudPolygon.vertex[ n_leftpoint ].n_coord ) << 16 ) - n_cx1.d ) / n_distance;
			n_dr1 = (int32_t)( ( BGR_R( m_packet.GouraudPolygon.vertex[ n_leftpoint ].n_bgr ) << 16 ) - n_cr1.d ) / n_distance;
			n_dg1 = (int32_t)( ( BGR_G( m_packet.GouraudPolygon.vertex[ n_leftpoint ].n_bgr ) << 16 ) - n_cg1.d ) / n_distance;
			n_db1 = (int32_t)( ( BGR_B( m_packet.GouraudPolygon.vertex[ n_leftpoint ].n_bgr ) << 16 ) - n_cb1.d ) / n_distance;
		}

		if( n_y == COORD_Y( m_packet.GouraudPolygon.vertex[ n_rightpoint ].n_coord ) )
		{
			while( n_y == COORD_Y( m_packet.GouraudPolygon.vertex[ p_n_rightpointlist[ n_rightpoint ] ].n_coord ) )
			{
				n_rightpoint = p_n_rightpointlist[ n_rightpoint ];
				if( n_rightpoint == n_leftpoint )
				{
					break;
				}
			}

			n_cx2.sw.h = COORD_X( m_packet.GouraudPolygon.vertex[ n_rightpoint ].n_coord ); n_cx2.sw.l = 0;
			n_cr2.w.h = BGR_R( m_packet.GouraudPolygon.vertex[ n_rightpoint ].n_bgr ); n_cr2.w.l = 0;
			n_cg2.w.h = BGR_G( m_packet.GouraudPolygon.vertex[ n_rightpoint ].n_bgr ); n_cg2.w.l = 0;
			n_cb2.w.h = BGR_B( m_packet.GouraudPolygon.vertex[ n_rightpoint ].n_bgr ); n_cb2.w.l = 0;
			n_rightpoint = p_n_rightpointlist[ n_rightpoint ];

			int32_t n_distance = COORD_Y( m_packet.GouraudPolygon.vertex[ n_rightpoint ].n_coord ) - n_y;
			if( n_distance < 1 )
			{
				break;
			}

			n_dx2 = (int32_t)( ( COORD_X( m_packet.GouraudPolygon.vertex[ n_rightpoint ].n_coord ) << 16 ) - n_cx2.d ) / n_distance;
			n_dr2 = (int32_t)( ( BGR_R( m_packet.GouraudPolygon.vertex[ n_rightpoint ].n_bgr ) << 16 ) - n_cr2.d ) / n_distance;
			n_dg2 = (int32_t)( ( BGR_G( m_packet.GouraudPolygon.vertex[ n_rightpoint ].n_bgr ) << 16 ) - n_cg2.d ) / n_distance;
			n_db2 = (int32_t)( ( BGR_B( m_packet.GouraudPolygon.vertex[ n_rightpoint ].n_bgr ) << 16 ) - n_cb2.d ) / n_distance;
		}

		int drawy = n_y + n_drawoffset_y;

		if( (int16_t)n_cx1.sw.h != (int16_t)n_cx2.sw.h && drawy >= (int32_t)n_drawarea_y1 && drawy <= (int32_t)n_drawarea_y2 )
		{
			int16_t n_x;
			int32_t n_distance;
			PAIR n_r;
			PAIR n_g;
			PAIR n_b;
			int32_t n_dr;
			int32_t n_dg;
			int32_t n_db;

			if( (int16_t)n_cx1.sw.h < (int16_t)n_cx2.sw.h )
			{
				n_x = n_cx1.sw.h;
				n_distance = (int16_t)n_cx2.sw.h - n_x;

				n_r.d = n_cr1.d;
				n_g.d = n_cg1.d;
				n_b.d = n_cb1.d;
				n_dr = (int32_t)( n_cr2.d - n_cr1.d ) / n_distance;
				n_dg = (int32_t)( n_cg2.d - n_cg1.d ) / n_distance;
				n_db = (int32_t)( n_cb2.d - n_cb1.d ) / n_distance;
			}
			else
			{
				n_x = n_cx2.sw.h;
				n_distance = (int16_t)n_cx1.sw.h - n_x;

				n_r.d = n_cr2.d;
				n_g.d = n_cg2.d;
				n_b.d = n_cb2.d;
				n_dr = (int32_t)( n_cr1.d - n_cr2.d ) / n_distance;
				n_dg = (int32_t)( n_cg1.d - n_cg2.d ) / n_distance;
				n_db = (int32_t)( n_cb1.d - n_cb2.d ) / n_distance;
			}

			int drawx = n_x + n_drawoffset_x;

			if( ( (int32_t)n_drawarea_x1 - drawx ) > 0 )
			{
				n_r.d += n_dr * ( n_drawarea_x1 - drawx );
				n_g.d += n_dg * ( n_drawarea_x1 - drawx );
				n_b.d += n_db * ( n_drawarea_x1 - drawx );
				n_distance -= ( n_drawarea_x1 - drawx );
				drawx = n_drawarea_x1;
			}

			SOLIDFILL( GOURAUDPOLYGONUPDATE )
		}

		n_cx1.d += n_dx1;
		n_cr1.d += n_dr1;
		n_cg1.d += n_dg1;
		n_cb1.d += n_db1;
		n_cx2.d += n_dx2;
		n_cr2.d += n_dr2;
		n_cg2.d += n_dg2;
		n_cb2.d += n_db2;
		n_y++;
	}
}

void psxgpu_device::GouraudTexturedPolygon( int n_points )
{
	if( gpu_active() )
	{
		gpu_submit_gouraud_textured_polygon( n_points );
		return;
	}

#if PSXGPU_DEBUG_VIEWER
	if( m_debug.n_skip == 4 )
	{
		return;
	}
	for( int n_point = 0; n_point < n_points; n_point++ )
	{
		DebugMesh( S11_COORD_X( m_packet.GouraudTexturedPolygon.vertex[ n_point ].n_coord ) + n_drawoffset_x, S11_COORD_Y( m_packet.GouraudTexturedPolygon.vertex[ n_point ].n_coord ) + n_drawoffset_y );
	}
	DebugMeshEnd();
#endif

	uint8_t n_cmd = BGR_C( m_packet.GouraudTexturedPolygon.vertex[ 0 ].n_bgr );

	uint32_t n_clutx = ( m_packet.GouraudTexturedPolygon.vertex[ 0 ].n_texture.w.h & 0x3f ) << 4;
	uint32_t n_cluty = ( m_packet.GouraudTexturedPolygon.vertex[ 0 ].n_texture.w.h >> 6 ) & 0x3ff;

	PAIR n_cx1; n_cx1.d = 0;
	PAIR n_cr1; n_cr1.d = 0;
	PAIR n_cg1; n_cg1.d = 0;
	PAIR n_cb1; n_cb1.d = 0;
	PAIR n_cu1; n_cu1.d = 0;
	PAIR n_cv1; n_cv1.d = 0;
	PAIR n_cx2; n_cx2.d = 0;
	PAIR n_cr2; n_cr2.d = 0;
	PAIR n_cg2; n_cg2.d = 0;
	PAIR n_cb2; n_cb2.d = 0;
	PAIR n_cu2; n_cu2.d = 0;
	PAIR n_cv2; n_cv2.d = 0;

	decode_tpage( m_packet.GouraudTexturedPolygon.vertex[ 1 ].n_texture.w.h );
	TEXTURESETUP

	FINDTOPLEFT( GouraudTexturedPolygon )

	int32_t n_dx1 = 0;
	int32_t n_dx2 = 0;
	int32_t n_du1 = 0;
	int32_t n_du2 = 0;
	int32_t n_dr1 = 0;
	int32_t n_dr2 = 0;
	int32_t n_dg1 = 0;
	int32_t n_dg2 = 0;
	int32_t n_db1 = 0;
	int32_t n_db2 = 0;
	int32_t n_dv1 = 0;
	int32_t n_dv2 = 0;

	int16_t n_y = COORD_Y( m_packet.GouraudTexturedPolygon.vertex[ n_rightpoint ].n_coord );

	for( ;; )
	{
		if( n_y == COORD_Y( m_packet.GouraudTexturedPolygon.vertex[ n_leftpoint ].n_coord ) )
		{
			while( n_y == COORD_Y( m_packet.GouraudTexturedPolygon.vertex[ p_n_leftpointlist[ n_leftpoint ] ].n_coord ) )
			{
				n_leftpoint = p_n_leftpointlist[ n_leftpoint ];
				if( n_leftpoint == n_rightpoint )
				{
					break;
				}
			}

			n_cx1.sw.h = COORD_X( m_packet.GouraudTexturedPolygon.vertex[ n_leftpoint ].n_coord ); n_cx1.sw.l = 0;
			n_cr1.w.h = n_cmd & 0x01 ? 0x80 : BGR_R( m_packet.GouraudTexturedPolygon.vertex[ n_leftpoint ].n_bgr ); n_cr1.w.l = 0;
			n_cg1.w.h = n_cmd & 0x01 ? 0x80 : BGR_G( m_packet.GouraudTexturedPolygon.vertex[ n_leftpoint ].n_bgr ); n_cg1.w.l = 0;
			n_cb1.w.h = n_cmd & 0x01 ? 0x80 : BGR_B( m_packet.GouraudTexturedPolygon.vertex[ n_leftpoint ].n_bgr ); n_cb1.w.l = 0;
			n_cu1.w.h = TEXTURE_U( m_packet.GouraudTexturedPolygon.vertex[ n_leftpoint ].n_texture ); n_cu1.w.l = 0;
			n_cv1.w.h = TEXTURE_V( m_packet.GouraudTexturedPolygon.vertex[ n_leftpoint ].n_texture ); n_cv1.w.l = 0;
			n_leftpoint = p_n_leftpointlist[ n_leftpoint ];

			int32_t n_distance = COORD_Y( m_packet.GouraudTexturedPolygon.vertex[ n_leftpoint ].n_coord ) - n_y;
			if( n_distance < 1 )
			{
				break;
			}

			n_dx1 = (int32_t)( ( COORD_X( m_packet.GouraudTexturedPolygon.vertex[ n_leftpoint ].n_coord ) << 16 ) - n_cx1.d ) / n_distance;
			n_dr1 = n_cmd & 0x01 ? 0 : (int32_t)( ( BGR_R( m_packet.GouraudTexturedPolygon.vertex[ n_leftpoint ].n_bgr ) << 16 ) - n_cr1.d ) / n_distance;
			n_dg1 = n_cmd & 0x01 ? 0 : (int32_t)( ( BGR_G( m_packet.GouraudTexturedPolygon.vertex[ n_leftpoint ].n_bgr ) << 16 ) - n_cg1.d ) / n_distance;
			n_db1 = n_cmd & 0x01 ? 0 : (int32_t)( ( BGR_B( m_packet.GouraudTexturedPolygon.vertex[ n_leftpoint ].n_bgr ) << 16 ) - n_cb1.d ) / n_distance;
			n_du1 = (int32_t)( ( TEXTURE_U( m_packet.GouraudTexturedPolygon.vertex[ n_leftpoint ].n_texture ) << 16 ) - n_cu1.d ) / n_distance;
			n_dv1 = (int32_t)( ( TEXTURE_V( m_packet.GouraudTexturedPolygon.vertex[ n_leftpoint ].n_texture ) << 16 ) - n_cv1.d ) / n_distance;
		}

		if( n_y == COORD_Y( m_packet.GouraudTexturedPolygon.vertex[ n_rightpoint ].n_coord ) )
		{
			while( n_y == COORD_Y( m_packet.GouraudTexturedPolygon.vertex[ p_n_rightpointlist[ n_rightpoint ] ].n_coord ) )
			{
				n_rightpoint = p_n_rightpointlist[ n_rightpoint ];
				if( n_rightpoint == n_leftpoint )
				{
					break;
				}
			}

			n_cx2.sw.h = COORD_X( m_packet.GouraudTexturedPolygon.vertex[ n_rightpoint ].n_coord ); n_cx2.sw.l = 0;
			n_cr2.w.h = n_cmd & 0x01 ? 0x80 : BGR_R( m_packet.GouraudTexturedPolygon.vertex[ n_rightpoint ].n_bgr ); n_cr2.w.l = 0;
			n_cg2.w.h = n_cmd & 0x01 ? 0x80 : BGR_G( m_packet.GouraudTexturedPolygon.vertex[ n_rightpoint ].n_bgr ); n_cg2.w.l = 0;
			n_cb2.w.h = n_cmd & 0x01 ? 0x80 : BGR_B( m_packet.GouraudTexturedPolygon.vertex[ n_rightpoint ].n_bgr ); n_cb2.w.l = 0;
			n_cu2.w.h = TEXTURE_U( m_packet.GouraudTexturedPolygon.vertex[ n_rightpoint ].n_texture ); n_cu2.w.l = 0;
			n_cv2.w.h = TEXTURE_V( m_packet.GouraudTexturedPolygon.vertex[ n_rightpoint ].n_texture ); n_cv2.w.l = 0;
			n_rightpoint = p_n_rightpointlist[ n_rightpoint ];

			int32_t n_distance = COORD_Y( m_packet.GouraudTexturedPolygon.vertex[ n_rightpoint ].n_coord ) - n_y;
			if( n_distance < 1 )
			{
				break;
			}

			n_dx2 = (int32_t)( ( COORD_X( m_packet.GouraudTexturedPolygon.vertex[ n_rightpoint ].n_coord ) << 16 ) - n_cx2.d ) / n_distance;
			n_dr2 = n_cmd & 0x01 ? 0 : (int32_t)( ( BGR_R( m_packet.GouraudTexturedPolygon.vertex[ n_rightpoint ].n_bgr ) << 16 ) - n_cr2.d ) / n_distance;
			n_dg2 = n_cmd & 0x01 ? 0 : (int32_t)( ( BGR_G( m_packet.GouraudTexturedPolygon.vertex[ n_rightpoint ].n_bgr ) << 16 ) - n_cg2.d ) / n_distance;
			n_db2 = n_cmd & 0x01 ? 0 : (int32_t)( ( BGR_B( m_packet.GouraudTexturedPolygon.vertex[ n_rightpoint ].n_bgr ) << 16 ) - n_cb2.d ) / n_distance;
			n_du2 = (int32_t)( ( TEXTURE_U( m_packet.GouraudTexturedPolygon.vertex[ n_rightpoint ].n_texture ) << 16 ) - n_cu2.d ) / n_distance;
			n_dv2 = (int32_t)( ( TEXTURE_V( m_packet.GouraudTexturedPolygon.vertex[ n_rightpoint ].n_texture ) << 16 ) - n_cv2.d ) / n_distance;
		}

		int drawy = n_y + n_drawoffset_y;

		if( (int16_t)n_cx1.sw.h != (int16_t)n_cx2.sw.h && drawy >= (int32_t)n_drawarea_y1 && drawy <= (int32_t)n_drawarea_y2 )
		{
			int16_t n_x;
			int32_t n_distance;
			PAIR n_r;
			PAIR n_g;
			PAIR n_b;
			PAIR n_u;
			PAIR n_v;
			int32_t n_dr;
			int32_t n_dg;
			int32_t n_db;
			int32_t n_du;
			int32_t n_dv;

			if( (int16_t)n_cx1.sw.h < (int16_t)n_cx2.sw.h )
			{
				n_x = n_cx1.sw.h;
				n_distance = (int16_t)n_cx2.sw.h - n_x;

				n_r.d = n_cr1.d;
				n_g.d = n_cg1.d;
				n_b.d = n_cb1.d;
				n_u.d = n_cu1.d;
				n_v.d = n_cv1.d;
				n_dr = (int32_t)( n_cr2.d - n_cr1.d ) / n_distance;
				n_dg = (int32_t)( n_cg2.d - n_cg1.d ) / n_distance;
				n_db = (int32_t)( n_cb2.d - n_cb1.d ) / n_distance;
				n_du = (int32_t)( n_cu2.d - n_cu1.d ) / n_distance;
				n_dv = (int32_t)( n_cv2.d - n_cv1.d ) / n_distance;
			}
			else
			{
				n_x = n_cx2.sw.h;
				n_distance = (int16_t)n_cx1.sw.h - n_x;

				n_r.d = n_cr2.d;
				n_g.d = n_cg2.d;
				n_b.d = n_cb2.d;
				n_u.d = n_cu2.d;
				n_v.d = n_cv2.d;
				n_dr = (int32_t)( n_cr1.d - n_cr2.d ) / n_distance;
				n_dg = (int32_t)( n_cg1.d - n_cg2.d ) / n_distance;
				n_db = (int32_t)( n_cb1.d - n_cb2.d ) / n_distance;
				n_du = (int32_t)( n_cu1.d - n_cu2.d ) / n_distance;
				n_dv = (int32_t)( n_cv1.d - n_cv2.d ) / n_distance;
			}

			int drawx = n_x + n_drawoffset_x;

			if( ( (int32_t)n_drawarea_x1 - drawx ) > 0 )
			{
				n_r.d += n_dr * ( n_drawarea_x1 - drawx );
				n_g.d += n_dg * ( n_drawarea_x1 - drawx );
				n_b.d += n_db * ( n_drawarea_x1 - drawx );
				n_u.d += n_du * ( n_drawarea_x1 - drawx );
				n_v.d += n_dv * ( n_drawarea_x1 - drawx );
				n_distance -= ( n_drawarea_x1 - drawx );
				drawx = n_drawarea_x1;
			}

			TEXTUREFILL( GOURAUDTEXTUREDPOLYGONUPDATE, n_u.w.h, n_v.w.h );
		}

		n_cx1.d += n_dx1;
		n_cr1.d += n_dr1;
		n_cg1.d += n_dg1;
		n_cb1.d += n_db1;
		n_cu1.d += n_du1;
		n_cv1.d += n_dv1;
		n_cx2.d += n_dx2;
		n_cr2.d += n_dr2;
		n_cg2.d += n_dg2;
		n_cb2.d += n_db2;
		n_cu2.d += n_du2;
		n_cv2.d += n_dv2;
		n_y++;
	}
}

void psxgpu_device::MonochromeLine()
{
	if( gpu_active() )
	{
		uint8_t n_cmd = BGR_C( m_packet.MonochromeLine.n_bgr );
		float r = BGR_R( m_packet.MonochromeLine.n_bgr ) / 255.0f;
		float g = BGR_G( m_packet.MonochromeLine.n_bgr ) / 255.0f;
		float b = BGR_B( m_packet.MonochromeLine.n_bgr ) / 255.0f;
		gpu_submit_line( S11_COORD_X( m_packet.MonochromeLine.vertex[ 0 ].n_coord ), S11_COORD_Y( m_packet.MonochromeLine.vertex[ 0 ].n_coord ),
			S11_COORD_X( m_packet.MonochromeLine.vertex[ 1 ].n_coord ), S11_COORD_Y( m_packet.MonochromeLine.vertex[ 1 ].n_coord ),
			r, g, b, r, g, b, n_cmd );
		return;
	}

#if PSXGPU_DEBUG_VIEWER
	if( m_debug.n_skip == 5 )
	{
		return;
	}
	DebugMesh( S11_COORD_X( m_packet.MonochromeLine.vertex[ 0 ].n_coord ) + n_drawoffset_x, S11_COORD_Y( m_packet.MonochromeLine.vertex[ 0 ].n_coord ) + n_drawoffset_y );
	DebugMesh( S11_COORD_X( m_packet.MonochromeLine.vertex[ 1 ].n_coord ) + n_drawoffset_x, S11_COORD_Y( m_packet.MonochromeLine.vertex[ 1 ].n_coord ) + n_drawoffset_y );
	DebugMeshEnd();
#endif

	int32_t n_xstart = S11_COORD_X( m_packet.MonochromeLine.vertex[ 0 ].n_coord );
	int32_t n_xend = S11_COORD_X( m_packet.MonochromeLine.vertex[ 1 ].n_coord );
	int32_t n_ystart = S11_COORD_Y( m_packet.MonochromeLine.vertex[ 0 ].n_coord );
	int32_t n_yend = S11_COORD_Y( m_packet.MonochromeLine.vertex[ 1 ].n_coord );

	uint8_t n_cmd = BGR_C( m_packet.MonochromeLine.n_bgr );
	uint8_t n_r = BGR_R( m_packet.MonochromeLine.n_bgr );
	uint8_t n_g = BGR_G( m_packet.MonochromeLine.n_bgr );
	uint8_t n_b = BGR_B( m_packet.MonochromeLine.n_bgr );

	TRANSPARENCYSETUP

	int32_t n_xlen;
	if( n_xend > n_xstart )
	{
		n_xlen = n_xend - n_xstart;
	}
	else
	{
		n_xlen = n_xstart - n_xend;
	}

	int32_t n_ylen;
	if( n_yend > n_ystart )
	{
		n_ylen = n_yend - n_ystart;
	}
	else
	{
		n_ylen = n_ystart - n_yend;
	}

	int32_t n_len;
	if( n_xlen > n_ylen )
	{
		n_len = n_xlen;
	}
	else
	{
		n_len = n_ylen;
	}

	if( n_len == 0 )
	{
		n_len = 1;
	}

	PAIR n_x; n_x.sw.h = n_xstart; n_x.sw.l = 0;
	PAIR n_y; n_y.sw.h = n_ystart; n_y.sw.l = 0;

	int32_t n_dx = (int32_t)( ( n_xend << 16 ) - n_x.d ) / n_len;
	int32_t n_dy = (int32_t)( ( n_yend << 16 ) - n_y.d ) / n_len;

	while( n_len > 0 )
	{
		int drawx = n_x.sw.h + n_drawoffset_x;
		int drawy = n_y.sw.h + n_drawoffset_y;

		if( drawx >= (int32_t)n_drawarea_x1 && drawy >= (int32_t)n_drawarea_y1 &&
			drawx <= (int32_t)n_drawarea_x2 && drawy <= (int32_t)n_drawarea_y2 )
		{
			uint16_t *p_vram = p_p_vram[ drawy ] + drawx;

			switch( n_cmd & 0x02 )
			{
			case 0x00:
				/* transparency off */
				WRITE_PIXEL(
					p_n_redshade[ MID_LEVEL | n_r ] |
					p_n_greenshade[ MID_LEVEL | n_g ] |
					p_n_blueshade[ MID_LEVEL | n_b ] )
				break;
			case 0x02:
				/* transparency on */
				WRITE_PIXEL(
					p_n_redtrans[ p_n_f[ MID_LEVEL | n_r ] | p_n_redb[ *( p_vram ) ] ] |
					p_n_greentrans[ p_n_f[ MID_LEVEL | n_g ] | p_n_greenb[ *( p_vram ) ] ] |
					p_n_bluetrans[ p_n_f[ MID_LEVEL | n_b ] | p_n_blueb[ *( p_vram ) ] ] )
				break;
			}
		}

		n_x.d += n_dx;
		n_y.d += n_dy;
		n_len--;
	}
}

void psxgpu_device::GouraudLine()
{
	if( gpu_active() )
	{
		uint8_t n_cmd = BGR_C( m_packet.GouraudLine.vertex[ 0 ].n_bgr );
		gpu_submit_line( S11_COORD_X( m_packet.GouraudLine.vertex[ 0 ].n_coord ), S11_COORD_Y( m_packet.GouraudLine.vertex[ 0 ].n_coord ),
			S11_COORD_X( m_packet.GouraudLine.vertex[ 1 ].n_coord ), S11_COORD_Y( m_packet.GouraudLine.vertex[ 1 ].n_coord ),
			BGR_R( m_packet.GouraudLine.vertex[ 0 ].n_bgr ) / 255.0f, BGR_G( m_packet.GouraudLine.vertex[ 0 ].n_bgr ) / 255.0f, BGR_B( m_packet.GouraudLine.vertex[ 0 ].n_bgr ) / 255.0f,
			BGR_R( m_packet.GouraudLine.vertex[ 1 ].n_bgr ) / 255.0f, BGR_G( m_packet.GouraudLine.vertex[ 1 ].n_bgr ) / 255.0f, BGR_B( m_packet.GouraudLine.vertex[ 1 ].n_bgr ) / 255.0f,
			n_cmd );
		return;
	}

#if PSXGPU_DEBUG_VIEWER
	if( m_debug.n_skip == 6 )
	{
		return;
	}
	DebugMesh( S11_COORD_X( m_packet.GouraudLine.vertex[ 0 ].n_coord ) + n_drawoffset_x, S11_COORD_Y( m_packet.GouraudLine.vertex[ 0 ].n_coord ) + n_drawoffset_y );
	DebugMesh( S11_COORD_X( m_packet.GouraudLine.vertex[ 1 ].n_coord ) + n_drawoffset_x, S11_COORD_Y( m_packet.GouraudLine.vertex[ 1 ].n_coord ) + n_drawoffset_y );
	DebugMeshEnd();
#endif

	uint8_t n_cmd = BGR_C( m_packet.GouraudLine.vertex[ 0 ].n_bgr );

	TRANSPARENCYSETUP

	int32_t n_xstart = S11_COORD_X( m_packet.GouraudLine.vertex[ 0 ].n_coord );
	int32_t n_ystart = S11_COORD_Y( m_packet.GouraudLine.vertex[ 0 ].n_coord );
	PAIR n_cr1; n_cr1.w.h = BGR_R( m_packet.GouraudLine.vertex[ 0 ].n_bgr ); n_cr1.w.l = 0;
	PAIR n_cg1; n_cg1.w.h = BGR_G( m_packet.GouraudLine.vertex[ 0 ].n_bgr ); n_cg1.w.l = 0;
	PAIR n_cb1; n_cb1.w.h = BGR_B( m_packet.GouraudLine.vertex[ 0 ].n_bgr ); n_cb1.w.l = 0;

	int32_t n_xend = S11_COORD_X( m_packet.GouraudLine.vertex[ 1 ].n_coord );
	int32_t n_yend = S11_COORD_Y( m_packet.GouraudLine.vertex[ 1 ].n_coord );
	PAIR n_cr2; n_cr2.w.h = BGR_R( m_packet.GouraudLine.vertex[ 1 ].n_bgr ); n_cr2.w.l = 0;
	PAIR n_cg2; n_cg2.w.h = BGR_G( m_packet.GouraudLine.vertex[ 1 ].n_bgr ); n_cg2.w.l = 0;
	PAIR n_cb2; n_cb2.w.h = BGR_B( m_packet.GouraudLine.vertex[ 1 ].n_bgr ); n_cb2.w.l = 0;


	PAIR n_x; n_x.sw.h = n_xstart; n_x.sw.l = 0;
	PAIR n_y; n_y.sw.h = n_ystart; n_y.sw.l = 0;
	PAIR n_r; n_r.d = n_cr1.d;
	PAIR n_g; n_g.d = n_cg1.d;
	PAIR n_b; n_b.d = n_cb1.d;

	int32_t n_xlen;
	if( n_xend > n_xstart )
	{
		n_xlen = n_xend - n_xstart;
	}
	else
	{
		n_xlen = n_xstart - n_xend;
	}

	int32_t n_ylen;
	if( n_yend > n_ystart )
	{
		n_ylen = n_yend - n_ystart;
	}
	else
	{
		n_ylen = n_ystart - n_yend;
	}

	int32_t n_distance;
	if( n_xlen > n_ylen )
	{
		n_distance = n_xlen;
	}
	else
	{
		n_distance = n_ylen;
	}

	if( n_distance == 0 )
	{
		n_distance = 1;
	}

	int32_t n_dx = (int32_t)( ( n_xend << 16 ) - n_x.sd ) / n_distance;
	int32_t n_dy = (int32_t)( ( n_yend << 16 ) - n_y.sd ) / n_distance;
	int32_t n_dr = (int32_t)( n_cr2.d - n_cr1.d ) / n_distance;
	int32_t n_dg = (int32_t)( n_cg2.d - n_cg1.d ) / n_distance;
	int32_t n_db = (int32_t)( n_cb2.d - n_cb1.d ) / n_distance;

	while( n_distance > 0 )
	{
		int drawx = n_x.sw.h + n_drawoffset_x;
		int drawy = n_y.sw.h + n_drawoffset_y;

		if( drawx >= (int32_t)n_drawarea_x1 && drawy >= (int32_t)n_drawarea_y1 &&
			drawx <= (int32_t)n_drawarea_x2 && drawy <= (int32_t)n_drawarea_y2 )
		{
			uint16_t *p_vram = p_p_vram[ drawy ] + drawx;

			switch( n_cmd & 0x02 )
			{
			case 0x00:
				/* transparency off */
				WRITE_PIXEL(
					p_n_redshade[ MID_LEVEL | n_r.w.h ] |
					p_n_greenshade[ MID_LEVEL | n_g.w.h ] |
					p_n_blueshade[ MID_LEVEL | n_b.w.h ] )
				break;
			case 0x02:
				/* transparency on */
				WRITE_PIXEL(
					p_n_redtrans[ p_n_f[ MID_LEVEL | n_r.w.h ] | p_n_redb[ *( p_vram ) ] ] |
					p_n_greentrans[ p_n_f[ MID_LEVEL | n_g.w.h ] | p_n_greenb[ *( p_vram ) ] ] |
					p_n_bluetrans[ p_n_f[ MID_LEVEL | n_b.w.h ] | p_n_blueb[ *( p_vram ) ] ] )
				break;
			}
		}

		n_x.sd += n_dx;
		n_y.sd += n_dy;
		n_r.d += n_dr;
		n_g.d += n_dg;
		n_b.d += n_db;
		n_distance--;
	}
}

void psxgpu_device::FrameBufferRectangleDraw()
{
	if( gpu_active() )
	{
		gpu_submit_vram_fill_rectangle( COORD_X( m_packet.FlatRectangle.n_coord ), COORD_Y( m_packet.FlatRectangle.n_coord ),
			SIZE_W( m_packet.FlatRectangle.n_size ), SIZE_H( m_packet.FlatRectangle.n_size ), m_packet.FlatRectangle.n_bgr );
		return;
	}

#if PSXGPU_DEBUG_VIEWER
	if( m_debug.n_skip == 7 )
	{
		return;
	}
	DebugMesh( S11_COORD_X( m_packet.FlatRectangle.n_coord ), S11_COORD_Y( m_packet.FlatRectangle.n_coord ) );
	DebugMesh( S11_COORD_X( m_packet.FlatRectangle.n_coord ) + SIZE_W( m_packet.FlatRectangle.n_size ), S11_COORD_Y( m_packet.FlatRectangle.n_coord ) );
	DebugMesh( S11_COORD_X( m_packet.FlatRectangle.n_coord ), S11_COORD_Y( m_packet.FlatRectangle.n_coord ) + SIZE_H( m_packet.FlatRectangle.n_size ) );
	DebugMesh( S11_COORD_X( m_packet.FlatRectangle.n_coord ) + SIZE_W( m_packet.FlatRectangle.n_size ), S11_COORD_Y( m_packet.FlatRectangle.n_coord ) + SIZE_H( m_packet.FlatRectangle.n_size ) );
	DebugMeshEnd();
#endif

	PAIR n_r; n_r.w.h = BGR_R( m_packet.FlatRectangle.n_bgr ); n_r.w.l = 0;
	PAIR n_g; n_g.w.h = BGR_G( m_packet.FlatRectangle.n_bgr ); n_g.w.l = 0;
	PAIR n_b; n_b.w.h = BGR_B( m_packet.FlatRectangle.n_bgr ); n_b.w.l = 0;

	int16_t n_y = COORD_Y( m_packet.FlatRectangle.n_coord );
	int32_t n_h = SIZE_H( m_packet.FlatRectangle.n_size );

	while( n_h > 0 )
	{
		int16_t n_x = COORD_X( m_packet.FlatRectangle.n_coord );
		int32_t n_distance = SIZE_W( m_packet.FlatRectangle.n_size );

		while( n_distance > 0 )
		{
			p_p_vram[ n_y & 1023 ][ n_x & 1023 ] =
				p_n_redshade[ MID_LEVEL | n_r.w.h ] |
				p_n_greenshade[ MID_LEVEL | n_g.w.h ] |
				p_n_blueshade[ MID_LEVEL | n_b.w.h ];
			n_x++;
			n_distance--;
		}

		n_y++;
		n_h--;
	}
}

void psxgpu_device::FlatRectangle()
{
	if( gpu_active() )
	{
		gpu_submit_flat_rectangle( S11_COORD_X( m_packet.FlatRectangle.n_coord ), S11_COORD_Y( m_packet.FlatRectangle.n_coord ),
			SIZE_W( m_packet.FlatRectangle.n_size ), SIZE_H( m_packet.FlatRectangle.n_size ), m_packet.FlatRectangle.n_bgr );
		return;
	}

#if PSXGPU_DEBUG_VIEWER
	if( m_debug.n_skip == 8 )
	{
		return;
	}
	DebugMesh( S11_COORD_X( m_packet.FlatRectangle.n_coord ) + n_drawoffset_x, S11_COORD_Y( m_packet.FlatRectangle.n_coord ) + n_drawoffset_y );
	DebugMesh( S11_COORD_X( m_packet.FlatRectangle.n_coord ) + n_drawoffset_x + SIZE_W( m_packet.FlatRectangle.n_size ), S11_COORD_Y( m_packet.FlatRectangle.n_coord ) + n_drawoffset_y );
	DebugMesh( S11_COORD_X( m_packet.FlatRectangle.n_coord ) + n_drawoffset_x, S11_COORD_Y( m_packet.FlatRectangle.n_coord ) + n_drawoffset_y + SIZE_H( m_packet.FlatRectangle.n_size ) );
	DebugMesh( S11_COORD_X( m_packet.FlatRectangle.n_coord ) + n_drawoffset_x + SIZE_W( m_packet.FlatRectangle.n_size ), S11_COORD_Y( m_packet.FlatRectangle.n_coord ) + n_drawoffset_y + SIZE_H( m_packet.FlatRectangle.n_size ) );
	DebugMeshEnd();
#endif

	uint8_t n_cmd = BGR_C( m_packet.FlatRectangle.n_bgr );

	SOLIDSETUP

	PAIR n_r; n_r.w.h = BGR_R( m_packet.FlatRectangle.n_bgr ); n_r.w.l = 0;
	PAIR n_g; n_g.w.h = BGR_G( m_packet.FlatRectangle.n_bgr ); n_g.w.l = 0;
	PAIR n_b; n_b.w.h = BGR_B( m_packet.FlatRectangle.n_bgr ); n_b.w.l = 0;

	int16_t n_x = S11_COORD_X( m_packet.FlatRectangle.n_coord );
	int16_t n_y = S11_COORD_Y( m_packet.FlatRectangle.n_coord );
	int32_t n_h = SIZE_H( m_packet.FlatRectangle.n_size );

	while( n_h > 0 )
	{
		int32_t n_distance = SIZE_W( m_packet.FlatRectangle.n_size );
		int drawy = n_y + n_drawoffset_y;

		if( n_distance > 0 && drawy >= (int32_t)n_drawarea_y1 && drawy <= (int32_t)n_drawarea_y2 )
		{
			int drawx = n_x + n_drawoffset_x;

			if( ( (int32_t)n_drawarea_x1 - drawx ) > 0 )
			{
				n_distance -= ( n_drawarea_x1 - drawx );
				drawx = n_drawarea_x1;
			}

			SOLIDFILL( FLATRECTANGEUPDATE )
		}

		n_y++;
		n_h--;
	}
}

void psxgpu_device::FlatRectangle8x8()
{
	if( gpu_active() )
	{
		gpu_submit_flat_rectangle( S11_COORD_X( m_packet.FlatRectangle8x8.n_coord ), S11_COORD_Y( m_packet.FlatRectangle8x8.n_coord ),
			8, 8, m_packet.FlatRectangle8x8.n_bgr );
		return;
	}

#if PSXGPU_DEBUG_VIEWER
	if( m_debug.n_skip == 9 )
	{
		return;
	}
	DebugMesh( S11_COORD_X( m_packet.FlatRectangle8x8.n_coord ) + n_drawoffset_x, S11_COORD_Y( m_packet.FlatRectangle8x8.n_coord ) + n_drawoffset_y );
	DebugMesh( S11_COORD_X( m_packet.FlatRectangle8x8.n_coord ) + n_drawoffset_x + 8, S11_COORD_Y( m_packet.FlatRectangle8x8.n_coord ) + n_drawoffset_y );
	DebugMesh( S11_COORD_X( m_packet.FlatRectangle8x8.n_coord ) + n_drawoffset_x, S11_COORD_Y( m_packet.FlatRectangle8x8.n_coord ) + n_drawoffset_y + 8 );
	DebugMesh( S11_COORD_X( m_packet.FlatRectangle8x8.n_coord ) + n_drawoffset_x + 8, S11_COORD_Y( m_packet.FlatRectangle8x8.n_coord ) + n_drawoffset_y + 8 );
	DebugMeshEnd();
#endif

	uint8_t n_cmd = BGR_C( m_packet.FlatRectangle8x8.n_bgr );

	SOLIDSETUP

	PAIR n_r; n_r.w.h = BGR_R( m_packet.FlatRectangle8x8.n_bgr ); n_r.w.l = 0;
	PAIR n_g; n_g.w.h = BGR_G( m_packet.FlatRectangle8x8.n_bgr ); n_g.w.l = 0;
	PAIR n_b; n_b.w.h = BGR_B( m_packet.FlatRectangle8x8.n_bgr ); n_b.w.l = 0;

	int16_t n_x = S11_COORD_X( m_packet.FlatRectangle8x8.n_coord );
	int16_t n_y = S11_COORD_Y( m_packet.FlatRectangle8x8.n_coord );
	int32_t n_h = 8;

	while( n_h > 0 )
	{
		int32_t n_distance = 8;
		int drawy = n_y + n_drawoffset_y;

		if( n_distance > 0 && drawy >= (int32_t)n_drawarea_y1 && drawy <= (int32_t)n_drawarea_y2 )
		{
			int drawx = n_x + n_drawoffset_x;

			if( ( (int32_t)n_drawarea_x1 - drawx ) > 0 )
			{
				n_distance -= ( n_drawarea_x1 - drawx );
				drawx = n_drawarea_x1;
			}

			SOLIDFILL( FLATRECTANGEUPDATE )
		}

		n_y++;
		n_h--;
	}
}

void psxgpu_device::FlatRectangle16x16()
{
	if( gpu_active() )
	{
		gpu_submit_flat_rectangle( S11_COORD_X( m_packet.FlatRectangle16x16.n_coord ), S11_COORD_Y( m_packet.FlatRectangle16x16.n_coord ),
			16, 16, m_packet.FlatRectangle16x16.n_bgr );
		return;
	}

#if PSXGPU_DEBUG_VIEWER
	if( m_debug.n_skip == 10 )
	{
		return;
	}
	DebugMesh( S11_COORD_X( m_packet.FlatRectangle16x16.n_coord ) + n_drawoffset_x, S11_COORD_Y( m_packet.FlatRectangle16x16.n_coord ) + n_drawoffset_y );
	DebugMesh( S11_COORD_X( m_packet.FlatRectangle16x16.n_coord ) + n_drawoffset_x + 16, S11_COORD_Y( m_packet.FlatRectangle16x16.n_coord ) + n_drawoffset_y );
	DebugMesh( S11_COORD_X( m_packet.FlatRectangle16x16.n_coord ) + n_drawoffset_x, S11_COORD_Y( m_packet.FlatRectangle16x16.n_coord ) + n_drawoffset_y + 16 );
	DebugMesh( S11_COORD_X( m_packet.FlatRectangle16x16.n_coord ) + n_drawoffset_x + 16, S11_COORD_Y( m_packet.FlatRectangle16x16.n_coord ) + n_drawoffset_y + 16 );
	DebugMeshEnd();
#endif

	uint8_t n_cmd = BGR_C( m_packet.FlatRectangle16x16.n_bgr );

	SOLIDSETUP

	PAIR n_r; n_r.w.h = BGR_R( m_packet.FlatRectangle16x16.n_bgr ); n_r.w.l = 0;
	PAIR n_g; n_g.w.h = BGR_G( m_packet.FlatRectangle16x16.n_bgr ); n_g.w.l = 0;
	PAIR n_b; n_b.w.h = BGR_B( m_packet.FlatRectangle16x16.n_bgr ); n_b.w.l = 0;

	int16_t n_x = S11_COORD_X( m_packet.FlatRectangle16x16.n_coord );
	int16_t n_y = S11_COORD_Y( m_packet.FlatRectangle16x16.n_coord );
	int32_t n_h = 16;

	while( n_h > 0 )
	{
		int32_t n_distance = 16;
		int drawy = n_y + n_drawoffset_y;

		if( n_distance > 0 && n_y >= (int32_t)n_drawarea_y1 && n_y <= (int32_t)n_drawarea_y2 )
		{
			int drawx = n_x + n_drawoffset_x;

			if( ( (int32_t)n_drawarea_x1 - drawx ) > 0 )
			{
				n_distance -= ( n_drawarea_x1 - drawx );
				drawx = n_drawarea_x1;
			}

			SOLIDFILL( FLATRECTANGEUPDATE )
		}

		n_y++;
		n_h--;
	}
}

void psxgpu_device::FlatTexturedRectangle()
{
	if( gpu_active() )
	{
		uint32_t n_clutx = ( m_packet.FlatTexturedRectangle.n_texture.w.h & 0x3f ) << 4;
		uint32_t n_cluty = ( m_packet.FlatTexturedRectangle.n_texture.w.h >> 6 ) & 0x3ff;
		int n_tx = m_n_tx;
		int n_ty = m_n_ty;
		switch( n_tp )
		{
		case 0: n_tx += n_twx >> 2; n_ty += n_twy; break;
		case 1: n_tx += n_twx >> 1; n_ty += n_twy; break;
		case 2: n_tx += n_twx >> 0; n_ty += n_twy; break;
		}
		gpu_submit_textured_rectangle( S11_COORD_X( m_packet.FlatTexturedRectangle.n_coord ), S11_COORD_Y( m_packet.FlatTexturedRectangle.n_coord ),
			SIZE_W( m_packet.FlatTexturedRectangle.n_size ), SIZE_H( m_packet.FlatTexturedRectangle.n_size ),
			TEXTURE_U( m_packet.FlatTexturedRectangle.n_texture ), TEXTURE_V( m_packet.FlatTexturedRectangle.n_texture ),
			m_packet.FlatTexturedRectangle.n_bgr, n_tx, n_ty, n_tp, n_clutx, n_cluty );
		return;
	}

#if PSXGPU_DEBUG_VIEWER
	if( m_debug.n_skip == 11 )
	{
		return;
	}
	DebugMesh( S11_COORD_X( m_packet.FlatTexturedRectangle.n_coord ) + n_drawoffset_x, S11_COORD_Y( m_packet.FlatTexturedRectangle.n_coord ) + n_drawoffset_y );
	DebugMesh( S11_COORD_X( m_packet.FlatTexturedRectangle.n_coord ) + n_drawoffset_x + SIZE_W( m_packet.FlatTexturedRectangle.n_size ), S11_COORD_Y( m_packet.FlatTexturedRectangle.n_coord ) + n_drawoffset_y );
	DebugMesh( S11_COORD_X( m_packet.FlatTexturedRectangle.n_coord ) + n_drawoffset_x, S11_COORD_Y( m_packet.FlatTexturedRectangle.n_coord ) + n_drawoffset_y + SIZE_H( m_packet.FlatTexturedRectangle.n_size ) );
	DebugMesh( S11_COORD_X( m_packet.FlatTexturedRectangle.n_coord ) + n_drawoffset_x + SIZE_W( m_packet.FlatTexturedRectangle.n_size ), S11_COORD_Y( m_packet.FlatTexturedRectangle.n_coord ) + n_drawoffset_y + SIZE_H( m_packet.FlatTexturedRectangle.n_size ) );
	DebugMeshEnd();
#endif

	uint8_t n_cmd = BGR_C( m_packet.FlatTexturedRectangle.n_bgr );

	uint32_t n_clutx = ( m_packet.FlatTexturedRectangle.n_texture.w.h & 0x3f ) << 4;
	uint32_t n_cluty = ( m_packet.FlatTexturedRectangle.n_texture.w.h >> 6 ) & 0x3ff;

	TEXTURESETUP
	SPRITESETUP

	PAIR n_r; n_r.w.h = n_cmd & 0x01 ? 0x80 : BGR_R( m_packet.FlatTexturedRectangle.n_bgr ); n_r.w.l = 0;
	PAIR n_g; n_g.w.h = n_cmd & 0x01 ? 0x80 : BGR_G( m_packet.FlatTexturedRectangle.n_bgr ); n_g.w.l = 0;
	PAIR n_b; n_b.w.h = n_cmd & 0x01 ? 0x80 : BGR_B( m_packet.FlatTexturedRectangle.n_bgr ); n_b.w.l = 0;

	int16_t n_x = S11_COORD_X( m_packet.FlatTexturedRectangle.n_coord );
	int16_t n_y = S11_COORD_Y( m_packet.FlatTexturedRectangle.n_coord );
	uint8_t n_v = TEXTURE_V( m_packet.FlatTexturedRectangle.n_texture );
	uint32_t n_h = SIZE_H( m_packet.FlatTexturedRectangle.n_size );

	while( n_h > 0 )
	{
		uint8_t n_u = TEXTURE_U( m_packet.FlatTexturedRectangle.n_texture );
		int16_t n_distance = SIZE_W( m_packet.FlatTexturedRectangle.n_size );
		int drawy = n_y + n_drawoffset_y;

		if( n_distance > 0 && drawy >= (int32_t)n_drawarea_y1 && drawy <= (int32_t)n_drawarea_y2 )
		{
			int drawx = n_x + n_drawoffset_x;

			if( ( (int32_t)n_drawarea_x1 - drawx ) > 0 )
			{
				n_u += ( n_drawarea_x1 - drawx ) * n_du;
				n_distance -= ( n_drawarea_x1 - drawx );
				drawx = n_drawarea_x1;
			}

			TEXTUREFILL( FLATTEXTUREDRECTANGLEUPDATE, n_u, n_v );
		}

		n_v += n_dv;
		n_y++;
		n_h--;
	}
}

void psxgpu_device::Sprite8x8()
{
	if( gpu_active() )
	{
		uint32_t n_clutx = ( m_packet.Sprite8x8.n_texture.w.h & 0x3f ) << 4;
		uint32_t n_cluty = ( m_packet.Sprite8x8.n_texture.w.h >> 6 ) & 0x3ff;
		int n_tx = m_n_tx;
		int n_ty = m_n_ty;
		switch( n_tp )
		{
		case 0: n_tx += n_twx >> 2; n_ty += n_twy; break;
		case 1: n_tx += n_twx >> 1; n_ty += n_twy; break;
		case 2: n_tx += n_twx >> 0; n_ty += n_twy; break;
		}
		gpu_submit_textured_rectangle( S11_COORD_X( m_packet.Sprite8x8.n_coord ), S11_COORD_Y( m_packet.Sprite8x8.n_coord ),
			8, 8, TEXTURE_U( m_packet.Sprite8x8.n_texture ), TEXTURE_V( m_packet.Sprite8x8.n_texture ),
			m_packet.Sprite8x8.n_bgr, n_tx, n_ty, n_tp, n_clutx, n_cluty );
		return;
	}

#if PSXGPU_DEBUG_VIEWER
	if( m_debug.n_skip == 12 )
	{
		return;
	}
	DebugMesh( S11_COORD_X( m_packet.Sprite8x8.n_coord ) + n_drawoffset_x, S11_COORD_Y( m_packet.Sprite8x8.n_coord ) + n_drawoffset_y );
	DebugMesh( S11_COORD_X( m_packet.Sprite8x8.n_coord ) + n_drawoffset_x + 7, S11_COORD_Y( m_packet.Sprite8x8.n_coord ) + n_drawoffset_y );
	DebugMesh( S11_COORD_X( m_packet.Sprite8x8.n_coord ) + n_drawoffset_x, S11_COORD_Y( m_packet.Sprite8x8.n_coord ) + n_drawoffset_y + 7 );
	DebugMesh( S11_COORD_X( m_packet.Sprite8x8.n_coord ) + n_drawoffset_x + 7, S11_COORD_Y( m_packet.Sprite8x8.n_coord ) + n_drawoffset_y + 7 );
	DebugMeshEnd();
#endif

	uint8_t n_cmd = BGR_C( m_packet.Sprite8x8.n_bgr );

	uint32_t n_clutx = ( m_packet.Sprite8x8.n_texture.w.h & 0x3f ) << 4;
	uint32_t n_cluty = ( m_packet.Sprite8x8.n_texture.w.h >> 6 ) & 0x3ff;

	TEXTURESETUP
	SPRITESETUP

	PAIR n_r; n_r.w.h = n_cmd & 0x01 ? 0x80 : BGR_R( m_packet.Sprite8x8.n_bgr ); n_r.w.l = 0;
	PAIR n_g; n_g.w.h = n_cmd & 0x01 ? 0x80 : BGR_G( m_packet.Sprite8x8.n_bgr ); n_g.w.l = 0;
	PAIR n_b; n_b.w.h = n_cmd & 0x01 ? 0x80 : BGR_B( m_packet.Sprite8x8.n_bgr ); n_b.w.l = 0;

	int16_t n_x = S11_COORD_X( m_packet.Sprite8x8.n_coord );
	int16_t n_y = S11_COORD_Y( m_packet.Sprite8x8.n_coord );
	uint8_t n_v = TEXTURE_V( m_packet.Sprite8x8.n_texture );
	uint32_t n_h = 8;

	while( n_h > 0 )
	{
		uint8_t n_u = TEXTURE_U( m_packet.Sprite8x8.n_texture );
		int16_t n_distance = 8;

		int drawy = n_y + n_drawoffset_y;

		if( n_distance > 0 && drawy >= (int32_t)n_drawarea_y1 && drawy <= (int32_t)n_drawarea_y2 )
		{
			int drawx = n_x + n_drawoffset_x;

			if( ( (int32_t)n_drawarea_x1 - drawx ) > 0 )
			{
				n_u += ( n_drawarea_x1 - drawx ) * n_du;
				n_distance -= ( n_drawarea_x1 - drawx );
				drawx = n_drawarea_x1;
			}

			TEXTUREFILL( FLATTEXTUREDRECTANGLEUPDATE, n_u, n_v );
		}

		n_v += n_dv;
		n_y++;
		n_h--;
	}
}

void psxgpu_device::Sprite16x16()
{
	if( gpu_active() )
	{
		uint32_t n_clutx = ( m_packet.Sprite16x16.n_texture.w.h & 0x3f ) << 4;
		uint32_t n_cluty = ( m_packet.Sprite16x16.n_texture.w.h >> 6 ) & 0x3ff;
		int n_tx = m_n_tx;
		int n_ty = m_n_ty;
		switch( n_tp )
		{
		case 0: n_tx += n_twx >> 2; n_ty += n_twy; break;
		case 1: n_tx += n_twx >> 1; n_ty += n_twy; break;
		case 2: n_tx += n_twx >> 0; n_ty += n_twy; break;
		}
		gpu_submit_textured_rectangle( S11_COORD_X( m_packet.Sprite16x16.n_coord ), S11_COORD_Y( m_packet.Sprite16x16.n_coord ),
			16, 16, TEXTURE_U( m_packet.Sprite16x16.n_texture ), TEXTURE_V( m_packet.Sprite16x16.n_texture ),
			m_packet.Sprite16x16.n_bgr, n_tx, n_ty, n_tp, n_clutx, n_cluty );
		return;
	}

#if PSXGPU_DEBUG_VIEWER
	if( m_debug.n_skip == 13 )
	{
		return;
	}
	DebugMesh( S11_COORD_X( m_packet.Sprite16x16.n_coord ) + n_drawoffset_x, S11_COORD_Y( m_packet.Sprite16x16.n_coord ) + n_drawoffset_y );
	DebugMesh( S11_COORD_X( m_packet.Sprite16x16.n_coord ) + n_drawoffset_x + 7, S11_COORD_Y( m_packet.Sprite16x16.n_coord ) + n_drawoffset_y );
	DebugMesh( S11_COORD_X( m_packet.Sprite16x16.n_coord ) + n_drawoffset_x, S11_COORD_Y( m_packet.Sprite16x16.n_coord ) + n_drawoffset_y + 7 );
	DebugMesh( S11_COORD_X( m_packet.Sprite16x16.n_coord ) + n_drawoffset_x + 7, S11_COORD_Y( m_packet.Sprite16x16.n_coord ) + n_drawoffset_y + 7 );
	DebugMeshEnd();
#endif

	uint8_t n_cmd = BGR_C( m_packet.Sprite16x16.n_bgr );

	uint32_t n_clutx = ( m_packet.Sprite16x16.n_texture.w.h & 0x3f ) << 4;
	uint32_t n_cluty = ( m_packet.Sprite16x16.n_texture.w.h >> 6 ) & 0x3ff;

	TEXTURESETUP
	SPRITESETUP

	PAIR n_r; n_r.w.h = n_cmd & 0x01 ? 0x80 : BGR_R( m_packet.Sprite16x16.n_bgr ); n_r.w.l = 0;
	PAIR n_g; n_g.w.h = n_cmd & 0x01 ? 0x80 : BGR_G( m_packet.Sprite16x16.n_bgr ); n_g.w.l = 0;
	PAIR n_b; n_b.w.h = n_cmd & 0x01 ? 0x80 : BGR_B( m_packet.Sprite16x16.n_bgr ); n_b.w.l = 0;

	int16_t n_x = S11_COORD_X( m_packet.Sprite16x16.n_coord );
	int16_t n_y = S11_COORD_Y( m_packet.Sprite16x16.n_coord );
	uint8_t n_v = TEXTURE_V( m_packet.Sprite16x16.n_texture );
	uint32_t n_h = 16;

	while( n_h > 0 )
	{
		uint8_t n_u = TEXTURE_U( m_packet.Sprite16x16.n_texture );
		int16_t n_distance = 16;

		int drawy = n_y + n_drawoffset_y;

		if( n_distance > 0 && drawy >= (int32_t)n_drawarea_y1 && drawy <= (int32_t)n_drawarea_y2 )
		{
			int drawx = n_x + n_drawoffset_x;

			if( ( (int32_t)n_drawarea_x1 - drawx ) > 0 )
			{
				n_u += ( n_drawarea_x1 - drawx ) * n_du;
				n_distance -= ( n_drawarea_x1 - drawx );
				drawx = n_drawarea_x1;
			}

			TEXTUREFILL( FLATTEXTUREDRECTANGLEUPDATE, n_u, n_v );
		}

		n_v += n_dv;
		n_y++;
		n_h--;
	}
}

void psxgpu_device::Dot()
{
	if( gpu_active() )
	{
		gpu_submit_dot( S11_COORD_X( m_packet.Dot.vertex.n_coord ), S11_COORD_Y( m_packet.Dot.vertex.n_coord ), m_packet.Dot.n_bgr );
		return;
	}

#if PSXGPU_DEBUG_VIEWER
	if( m_debug.n_skip == 14 )
	{
		return;
	}
	DebugMesh( S11_COORD_X( m_packet.Dot.vertex.n_coord ) + n_drawoffset_x, S11_COORD_Y( m_packet.Dot.vertex.n_coord ) + n_drawoffset_y );
	DebugMeshEnd();
#endif

	uint8_t n_cmd = BGR_C( m_packet.Dot.n_bgr );
	uint8_t n_r = BGR_R( m_packet.Dot.n_bgr );
	uint8_t n_g = BGR_G( m_packet.Dot.n_bgr );
	uint8_t n_b = BGR_B( m_packet.Dot.n_bgr );
	int32_t n_x = S11_COORD_X( m_packet.Dot.vertex.n_coord );
	int32_t n_y = S11_COORD_Y( m_packet.Dot.vertex.n_coord );

	TRANSPARENCYSETUP

	int drawx = n_x + n_drawoffset_x;
	int drawy = n_y + n_drawoffset_y;

	if( drawx >= (int32_t)n_drawarea_x1 && drawy >= (int32_t)n_drawarea_y1 &&
		drawx <= (int32_t)n_drawarea_x2 && drawy <= (int32_t)n_drawarea_y2 )
	{
		uint16_t *p_vram = p_p_vram[ drawy ] + drawx;

		switch( n_cmd & 0x02 )
		{
		case 0x00:
			/* transparency off */
			WRITE_PIXEL(
				p_n_redshade[ MID_LEVEL | n_r ] |
				p_n_greenshade[ MID_LEVEL | n_g ] |
				p_n_blueshade[ MID_LEVEL | n_b ] )
			break;
		case 0x02:
			/* transparency on */
			WRITE_PIXEL(
				p_n_redtrans[ p_n_f[ MID_LEVEL | n_r ] | p_n_redb[ *( p_vram ) ] ] |
				p_n_greentrans[ p_n_f[ MID_LEVEL | n_g ] | p_n_greenb[ *( p_vram ) ] ] |
				p_n_bluetrans[ p_n_f[ MID_LEVEL | n_b ] | p_n_blueb[ *( p_vram ) ] ] )
			break;
		}
	}
}

void psxgpu_device::TexturedDot()
{
	if( gpu_active() )
	{
		uint32_t n_clutx = ( m_packet.TexturedDot.vertex.n_texture.w.h & 0x3f ) << 4;
		uint32_t n_cluty = ( m_packet.TexturedDot.vertex.n_texture.w.h >> 6 ) & 0x3ff;
		int n_tx = m_n_tx;
		int n_ty = m_n_ty;
		switch( n_tp )
		{
		case 0: n_tx += n_twx >> 2; n_ty += n_twy; break;
		case 1: n_tx += n_twx >> 1; n_ty += n_twy; break;
		case 2: n_tx += n_twx >> 0; n_ty += n_twy; break;
		}
		gpu_submit_textured_dot( S11_COORD_X( m_packet.TexturedDot.vertex.n_coord ), S11_COORD_Y( m_packet.TexturedDot.vertex.n_coord ),
			TEXTURE_U( m_packet.TexturedDot.vertex.n_texture ), TEXTURE_V( m_packet.TexturedDot.vertex.n_texture ),
			m_packet.TexturedDot.n_bgr, n_tx, n_ty, n_tp, n_clutx, n_cluty );
		return;
	}

#if PSXGPU_DEBUG_VIEWER
	if (m_debug.n_skip == 15)
	{
		return;
	}
	DebugMesh( S11_COORD_X( m_packet.TexturedDot.vertex.n_coord ) + n_drawoffset_x, S11_COORD_Y( m_packet.TexturedDot.vertex.n_coord ) + n_drawoffset_y );
	DebugMeshEnd();
#endif

	uint8_t n_cmd = BGR_C( m_packet.TexturedDot.n_bgr );

	PAIR n_r; n_r.w.h = n_cmd & 0x01 ? 0x80 : BGR_R( m_packet.TexturedDot.n_bgr ); n_r.w.l = 0;
	PAIR n_g; n_g.w.h = n_cmd & 0x01 ? 0x80 : BGR_G( m_packet.TexturedDot.n_bgr ); n_g.w.l = 0;
	PAIR n_b; n_b.w.h = n_cmd & 0x01 ? 0x80 : BGR_B( m_packet.TexturedDot.n_bgr ); n_b.w.l = 0;

	int32_t n_x = S11_COORD_X( m_packet.TexturedDot.vertex.n_coord );
	int32_t n_y = S11_COORD_Y( m_packet.TexturedDot.vertex.n_coord );
	uint8_t n_u = TEXTURE_U(m_packet.TexturedDot.vertex.n_texture );
	uint8_t n_v = TEXTURE_V(m_packet.TexturedDot.vertex.n_texture );
	uint32_t n_clutx = ( m_packet.TexturedDot.vertex.n_texture.w.h & 0x3f ) << 4;
	uint32_t n_cluty = ( m_packet.TexturedDot.vertex.n_texture.w.h >> 6 ) & 0x3ff;

	TEXTURESETUP

	int32_t n_distance = 1;

	int drawx = n_x + n_drawoffset_x;
	int drawy = n_y + n_drawoffset_y;

	if( drawx >= (int32_t)n_drawarea_x1 && drawy >= (int32_t)n_drawarea_y1 &&
		drawx <= (int32_t)n_drawarea_x2 && drawy <= (int32_t)n_drawarea_y2 )
	{
		TEXTUREFILL( {}, n_u, n_v );
	}
}

void psxgpu_device::MoveImage()
{
	if( gpu_active() )
	{
		gpu_queue_copy_rect( COORD_X( m_packet.MoveImage.vertex[ 0 ].n_coord ), COORD_Y( m_packet.MoveImage.vertex[ 0 ].n_coord ),
			COORD_X( m_packet.MoveImage.vertex[ 1 ].n_coord ), COORD_Y( m_packet.MoveImage.vertex[ 1 ].n_coord ),
			SIZE_W( m_packet.MoveImage.n_size ), SIZE_H( m_packet.MoveImage.n_size ) );
		return;
	}

#if PSXGPU_DEBUG_VIEWER
	if( m_debug.n_skip == 16 )
	{
		return;
	}
	DebugMesh( S11_COORD_X( m_packet.MoveImage.vertex[ 1 ].n_coord ), S11_COORD_Y( m_packet.MoveImage.vertex[ 1 ].n_coord ) );
	DebugMesh( S11_COORD_X( m_packet.MoveImage.vertex[ 1 ].n_coord ) + SIZE_W( m_packet.MoveImage.n_size ), S11_COORD_Y( m_packet.MoveImage.vertex[ 1 ].n_coord ) );
	DebugMesh( S11_COORD_X( m_packet.MoveImage.vertex[ 1 ].n_coord ), S11_COORD_Y( m_packet.MoveImage.vertex[ 1 ].n_coord ) + SIZE_H( m_packet.MoveImage.n_size ) );
	DebugMesh( S11_COORD_X( m_packet.MoveImage.vertex[ 1 ].n_coord ) + SIZE_W( m_packet.MoveImage.n_size ), S11_COORD_Y( m_packet.MoveImage.vertex[ 1 ].n_coord ) + SIZE_H( m_packet.MoveImage.n_size ) );
	DebugMeshEnd();
#endif

	int16_t n_srcy = COORD_Y( m_packet.MoveImage.vertex[ 0 ].n_coord );
	int16_t n_dsty = COORD_Y( m_packet.MoveImage.vertex[ 1 ].n_coord );
	int16_t n_h = SIZE_H( m_packet.MoveImage.n_size );

	while( n_h > 0 )
	{
		int16_t n_srcx = COORD_X( m_packet.MoveImage.vertex[ 0 ].n_coord );
		int16_t n_dstx = COORD_X( m_packet.MoveImage.vertex[ 1 ].n_coord );
		int16_t n_w = SIZE_W( m_packet.MoveImage.n_size );

		while( n_w > 0 )
		{
			uint16_t *p_vram = p_p_vram[ n_dsty & 1023 ] + ( n_dstx & 1023 );
			WRITE_PIXEL( *( p_p_vram[ n_srcy & 1023 ] + ( n_srcx & 1023 ) ) )
			n_srcx++;
			n_dstx++;
			n_w--;
		}

		n_srcy++;
		n_dsty++;
		n_h--;
	}
}

void psxgpu_device::dma_write( uint32_t *p_n_psxram, uint32_t n_address, int32_t n_size )
{
	gpu_write( &p_n_psxram[ n_address / 4 ], n_size, n_address );
}

void psxgpu_device::gpu_write( uint32_t *p_ram, int32_t n_size, uint32_t base_address )
{
	uint32_t n_word = 0;
	while( n_size > 0 )
	{
		uint32_t data = *( p_ram );

		LOG("PSX Packet #%u %08x\n", n_gpu_buffer_offset, data);
		m_packet.n_entry[ n_gpu_buffer_offset ] = data;

		// PGXP (CLAUDE.md Phase 4c): mirror this word's shadow into
		// m_packet_shadow[] at the exact same index, so gpu_vertex_xyw()
		// (called later, once this command's full packet is parsed) can
		// look it up by the vertex field's own n_entry[] index rather
		// than re-deriving a lookup key from the vertex's value. Only
		// possible when this call came from dma_write() (base_address !=
		// PGXP_NO_ADDRESS) - see gpu_write()'s declaration for why the
		// direct single-word GP0 port write can't provide one.
		if( m_gpu_pgxp_enabled && m_cpu != nullptr && base_address != PGXP_NO_ADDRESS )
		{
			float x, y, w;
			uint32_t word_address = base_address + n_word * 4;
			bool valid = m_cpu->pgxp_ram_shadow_query( word_address, data, x, y, w );
			m_packet_shadow[ n_gpu_buffer_offset ].valid = valid;
			if( valid )
			{
				m_packet_shadow[ n_gpu_buffer_offset ].x = x;
				m_packet_shadow[ n_gpu_buffer_offset ].y = y;
				m_packet_shadow[ n_gpu_buffer_offset ].w = w;
			}
		}
		else
		{
			m_packet_shadow[ n_gpu_buffer_offset ].valid = false;
		}

		switch( m_packet.n_entry[ 0 ] >> 24 )
		{
		case 0x00:
			LOGMASKED(LOG_WRITE, "%s: not handled: GPU Command 0x00: (%08x)\n", machine().describe_context(), data);
			break;
		case 0x01:
			LOGMASKED(LOG_WRITE, "%s: not handled: clear cache\n", machine().describe_context());
			break;
		case 0x02:
			if( n_gpu_buffer_offset < 2 )
			{
				n_gpu_buffer_offset++;
			}
			else
			{
				LOGMASKED(LOG_WRITE, "%s: %02x: frame buffer rectangle %u,%u %u,%u\n", machine().describe_context(), m_packet.n_entry[ 0 ] >> 24,
					m_packet.n_entry[ 1 ] & 0xffff, m_packet.n_entry[ 1 ] >> 16, m_packet.n_entry[ 2 ] & 0xffff, m_packet.n_entry[ 2 ] >> 16 );
				FrameBufferRectangleDraw();
				n_gpu_buffer_offset = 0;
			}
			break;
		case 0x20:
		case 0x21:
		case 0x22:
		case 0x23:
			if( n_gpu_buffer_offset < 3 )
			{
				n_gpu_buffer_offset++;
			}
			else
			{
				LOGMASKED(LOG_WRITE, machine().describe_context(), "%s: %02x: monochrome 3 point polygon\n", machine().describe_context(), m_packet.n_entry[ 0 ] >> 24);
				FlatPolygon( 3 );
				n_gpu_buffer_offset = 0;
			}
			break;
		case 0x24:
		case 0x25:
		case 0x26:
		case 0x27:
			if( n_gpu_buffer_offset < 6 )
			{
				n_gpu_buffer_offset++;
			}
			else
			{
				LOGMASKED(LOG_WRITE, "%s: %02x: textured 3 point polygon\n", machine().describe_context(), m_packet.n_entry[ 0 ] >> 24);
				FlatTexturedPolygon( 3 );
				n_gpu_buffer_offset = 0;
			}
			break;
		case 0x28:
		case 0x29:
		case 0x2a:
		case 0x2b:
			if( n_gpu_buffer_offset < 4 )
			{
				n_gpu_buffer_offset++;
			}
			else
			{
				LOGMASKED(LOG_WRITE, "%s: %02x: monochrome 4 point polygon\n", machine().describe_context(), m_packet.n_entry[ 0 ] >> 24);
				FlatPolygon( 4 );
				n_gpu_buffer_offset = 0;
			}
			break;
		case 0x2c:
		case 0x2d:
		case 0x2e:
		case 0x2f:
			if( n_gpu_buffer_offset < 8 )
			{
				n_gpu_buffer_offset++;
			}
			else
			{
				LOGMASKED(LOG_WRITE, "%s: %02x: textured 4 point polygon\n", machine().describe_context(), m_packet.n_entry[ 0 ] >> 24);
				FlatTexturedPolygon( 4 );
				n_gpu_buffer_offset = 0;
			}
			break;
		case 0x30:
		case 0x31:
		case 0x32:
		case 0x33:
			if( n_gpu_buffer_offset < 5 )
			{
				n_gpu_buffer_offset++;
			}
			else
			{
				LOGMASKED(LOG_WRITE, "%s: %02x: gouraud 3 point polygon\n", machine().describe_context(), m_packet.n_entry[ 0 ] >> 24 );
				GouraudPolygon( 3 );
				n_gpu_buffer_offset = 0;
			}
			break;
		case 0x34:
		case 0x35:
		case 0x36:
		case 0x37:
			if( n_gpu_buffer_offset < 8 )
			{
				n_gpu_buffer_offset++;
			}
			else
			{
				LOGMASKED(LOG_WRITE, "%s: %02x: gouraud textured 3 point polygon\n", machine().describe_context(), m_packet.n_entry[ 0 ] >> 24);
				GouraudTexturedPolygon( 3 );
				n_gpu_buffer_offset = 0;
			}
			break;
		case 0x38:
		case 0x39:
		case 0x3a:
		case 0x3b:
			if( n_gpu_buffer_offset < 7 )
			{
				n_gpu_buffer_offset++;
			}
			else
			{
				LOGMASKED(LOG_WRITE, "%s: %02x: gouraud 4 point polygon\n", machine().describe_context(), m_packet.n_entry[ 0 ] >> 24);
				GouraudPolygon( 4 );
				n_gpu_buffer_offset = 0;
			}
			break;
		case 0x3c:
		case 0x3d:
		case 0x3e:
		case 0x3f:
			if( n_gpu_buffer_offset < 11 )
			{
				n_gpu_buffer_offset++;
			}
			else
			{
				LOGMASKED(LOG_WRITE, "%s: %02x: gouraud textured 4 point polygon\n", machine().describe_context(), m_packet.n_entry[ 0 ] >> 24);
				GouraudTexturedPolygon( 4 );
				n_gpu_buffer_offset = 0;
			}
			break;
		case 0x40:
		case 0x41:
		case 0x42:
		case 0x43:
			if( n_gpu_buffer_offset < 2 )
			{
				n_gpu_buffer_offset++;
			}
			else
			{
				LOGMASKED(LOG_WRITE, "%s: %02x: monochrome line\n", machine().describe_context(), m_packet.n_entry[ 0 ] >> 24);
				MonochromeLine();
				n_gpu_buffer_offset = 0;
			}
			break;
		case 0x48:
		case 0x4a:
		case 0x4c:
		case 0x4e:
			if( n_gpu_buffer_offset < 3 )
			{
				n_gpu_buffer_offset++;
			}
			else
			{
				LOGMASKED(LOG_WRITE, "%s: %02x: monochrome polyline\n", machine().describe_context(), m_packet.n_entry[ 0 ] >> 24);
				MonochromeLine();
				if( ( m_packet.n_entry[ 3 ] & 0xf000f000 ) != 0x50005000 )
				{
					m_packet.n_entry[ 1 ] = m_packet.n_entry[ 2 ];
					m_packet.n_entry[ 2 ] = m_packet.n_entry[ 3 ];
					n_gpu_buffer_offset = 3;
				}
				else
				{
					n_gpu_buffer_offset = 0;
				}
			}
			break;
		case 0x50:
		case 0x51:
		case 0x52:
		case 0x53:
			if( n_gpu_buffer_offset < 3 )
			{
				n_gpu_buffer_offset++;
			}
			else
			{
				LOGMASKED(LOG_WRITE, "%s: %02x: gouraud line\n", machine().describe_context(), m_packet.n_entry[ 0 ] >> 24);
				GouraudLine();
				n_gpu_buffer_offset = 0;
			}
			break;
		case 0x58:
		case 0x5a:
		case 0x5c:
		case 0x5e:
			if( n_gpu_buffer_offset < 5 &&
				( n_gpu_buffer_offset != 4 || ( m_packet.n_entry[ 4 ] & 0xf000f000 ) != 0x50005000 ) )
			{
				n_gpu_buffer_offset++;
			}
			else
			{
				LOGMASKED(LOG_WRITE, "%s: %02x: gouraud polyline\n", machine().describe_context(), m_packet.n_entry[ 0 ] >> 24);
				GouraudLine();
				if( ( m_packet.n_entry[ 4 ] & 0xf000f000 ) != 0x50005000 )
				{
					m_packet.n_entry[ 0 ] = ( m_packet.n_entry[ 0 ] & 0xff000000 ) | ( m_packet.n_entry[ 2 ] & 0x00ffffff );
					m_packet.n_entry[ 1 ] = m_packet.n_entry[ 3 ];
					m_packet.n_entry[ 2 ] = m_packet.n_entry[ 4 ];
					m_packet.n_entry[ 3 ] = m_packet.n_entry[ 5 ];
					n_gpu_buffer_offset = 4;
				}
				else
				{
					n_gpu_buffer_offset = 0;
				}
			}
			break;
		case 0x60:
		case 0x61:
		case 0x62:
		case 0x63:
			if( n_gpu_buffer_offset < 2 )
			{
				n_gpu_buffer_offset++;
			}
			else
			{
				LOGMASKED(LOG_WRITE, "%s: 02x: rectangle %d,%d %d,%d\n", machine().describe_context(),
					m_packet.n_entry[ 0 ] >> 24,
					(int16_t)( m_packet.n_entry[ 1 ] & 0xffff ), (int16_t)( m_packet.n_entry[ 1 ] >> 16 ),
					(int16_t)( m_packet.n_entry[ 2 ] & 0xffff ), (int16_t)( m_packet.n_entry[ 2 ] >> 16 ) );
				FlatRectangle();
				n_gpu_buffer_offset = 0;
			}
			break;
		case 0x64:
		case 0x65:
		case 0x66:
		case 0x67:
			if( n_gpu_buffer_offset < 3 )
			{
				n_gpu_buffer_offset++;
			}
			else
			{
				LOGMASKED(LOG_WRITE, "%s: %02x: sprite %d,%d %u,%u %08x, %08x\n", machine().describe_context(),
					m_packet.n_entry[ 0 ] >> 24,
					(int16_t)( m_packet.n_entry[ 1 ] & 0xffff ), (int16_t)( m_packet.n_entry[ 1 ] >> 16 ),
					m_packet.n_entry[ 3 ] & 0xffff, m_packet.n_entry[ 3 ] >> 16,
					m_packet.n_entry[ 0 ], m_packet.n_entry[ 2 ] );
				FlatTexturedRectangle();
				n_gpu_buffer_offset = 0;
			}
			break;
		case 0x68:
		case 0x69:
		case 0x6a:
		case 0x6b:
			if( n_gpu_buffer_offset < 1 )
			{
				n_gpu_buffer_offset++;
			}
			else
			{
				LOGMASKED(LOG_WRITE, "%s: %02x: dot %d,%d %08x\n", machine().describe_context(),
					m_packet.n_entry[ 0 ] >> 24,
					(int16_t)( m_packet.n_entry[ 1 ] & 0xffff ), (int16_t)( m_packet.n_entry[ 1 ] >> 16 ),
					m_packet.n_entry[ 0 ] & 0xffffff );
				Dot();
				n_gpu_buffer_offset = 0;
			}
			break;
		case 0x6c:
		case 0x6d:
		case 0x6e:
		case 0x6f:
			if (n_gpu_buffer_offset < 2)
			{
				n_gpu_buffer_offset++;
			}
			else
			{
				LOGMASKED(LOG_WRITE, "%s: %02x: textured dot %d,%d %08x\n", machine().describe_context(),
					m_packet.n_entry[ 0 ] >> 24,
					(int16_t)( m_packet.n_entry[ 1 ] & 0xffff ), (int16_t)( m_packet.n_entry[ 1 ] >> 16 ),
					m_packet.n_entry[ 0 ] & 0xffffff );
				TexturedDot();
				n_gpu_buffer_offset = 0;
			}
			break;
		case 0x70:
		case 0x71:
		case 0x72:
		case 0x73:
			/* 8*8 rectangle */
			if( n_gpu_buffer_offset < 1 )
			{
				n_gpu_buffer_offset++;
			}
			else
			{
				LOGMASKED(LOG_WRITE, "%s; %02x: 16x16 rectangle %08x %08x\n", machine().describe_context(), m_packet.n_entry[ 0 ] >> 24,
					m_packet.n_entry[ 0 ], m_packet.n_entry[ 1 ] );
				FlatRectangle8x8();
				n_gpu_buffer_offset = 0;
			}
			break;
		case 0x74:
		case 0x75:
		case 0x76:
		case 0x77:
			if( n_gpu_buffer_offset < 2 )
			{
				n_gpu_buffer_offset++;
			}
			else
			{
				LOGMASKED(LOG_WRITE, "%s: %02x: 8x8 sprite %08x %08x %08x\n", machine().describe_context(), m_packet.n_entry[ 0 ] >> 24,
					m_packet.n_entry[ 0 ], m_packet.n_entry[ 1 ], m_packet.n_entry[ 2 ] );
				Sprite8x8();
				n_gpu_buffer_offset = 0;
			}
			break;
		case 0x78:
		case 0x79:
		case 0x7a:
		case 0x7b:
			/* 16*16 rectangle */
			if( n_gpu_buffer_offset < 1 )
			{
				n_gpu_buffer_offset++;
			}
			else
			{
				LOGMASKED(LOG_WRITE, "%s: %02x: 16x16 rectangle %08x %08x\n", machine().describe_context(), m_packet.n_entry[ 0 ] >> 24,
					m_packet.n_entry[ 0 ], m_packet.n_entry[ 1 ] );
				FlatRectangle16x16();
				n_gpu_buffer_offset = 0;
			}
			break;
		case 0x7c:
		case 0x7d:
		case 0x7e:
		case 0x7f:
			if( n_gpu_buffer_offset < 2 )
			{
				n_gpu_buffer_offset++;
			}
			else
			{
				LOGMASKED(LOG_WRITE, "%s: %02x: 16x16 sprite %08x %08x %08x\n", machine().describe_context(), m_packet.n_entry[ 0 ] >> 24,
					m_packet.n_entry[ 0 ], m_packet.n_entry[ 1 ], m_packet.n_entry[ 2 ] );
				Sprite16x16();
				n_gpu_buffer_offset = 0;
			}
			break;
		case 0x80:
			if( n_gpu_buffer_offset < 3 )
			{
				n_gpu_buffer_offset++;
			}
			else
			{
				LOGMASKED(LOG_WRITE, "%s: move image in frame buffer %08x %08x %08x %08x\n", machine().describe_context(),
					m_packet.n_entry[ 0 ], m_packet.n_entry[ 1 ], m_packet.n_entry[ 2 ], m_packet.n_entry[ 3 ]);
				MoveImage();
				n_gpu_buffer_offset = 0;
			}
			break;
		case 0xa0:
			if( n_gpu_buffer_offset < 3 )
			{
				n_gpu_buffer_offset++;
			}
			else
			{
				for( int n_pixel = 0; n_pixel < 2; n_pixel++ )
				{
					LOGMASKED(LOG_WRITE, "%s: send image to framebuffer ( pixel %u,%u = %u )\n",
						machine().describe_context(),
						( n_vramx + m_packet.n_entry[ 1 ] ) & 1023,
						( n_vramy + ( m_packet.n_entry[ 1 ] >> 16 ) ) & 1023,
						data & 0xffff );

					uint16_t *p_vram = p_p_vram[ ( n_vramy + ( m_packet.n_entry[ 1 ] >> 16 ) ) & 1023 ] + ( ( n_vramx + m_packet.n_entry[ 1 ] ) & 1023 );
					WRITE_PIXEL( data & 0xffff )
					n_vramx++;
					if( n_vramx >= ( m_packet.n_entry[ 2 ] & 0xffff ) )
					{
						n_vramx = 0;
						n_vramy++;
						if( n_vramy >= ( m_packet.n_entry[ 2 ] >> 16 ) )
						{
							LOGMASKED(LOG_WRITE, "%s: %02x: send image to framebuffer %u,%u %u,%u\n", machine().describe_context(), m_packet.n_entry[ 0 ] >> 24,
								m_packet.n_entry[ 1 ] & 0xffff, ( m_packet.n_entry[ 1 ] >> 16 ),
								m_packet.n_entry[ 2 ] & 0xffff, ( m_packet.n_entry[ 2 ] >> 16 ) );
							if( gpu_active() )
							{
								gpu_submit_image_stamp( m_packet.n_entry[ 1 ] & 0xffff, m_packet.n_entry[ 1 ] >> 16,
									m_packet.n_entry[ 2 ] & 0xffff, m_packet.n_entry[ 2 ] >> 16 );
							}
							n_gpu_buffer_offset = 0;
							n_vramx = 0;
							n_vramy = 0;
							break;
						}
					}
					data >>= 16;
				}
			}
			break;
		case 0xc0:
			if( n_gpu_buffer_offset < 2 )
			{
				n_gpu_buffer_offset++;
			}
			else
			{
				LOGMASKED(LOG_WRITE, "%s: %02x: copy image from frame buffer\n", machine().describe_context(), m_packet.n_entry[ 0 ] >> 24);
				n_gpustatus |= ( 1L << 0x1b );
			}
			break;
		case 0xe1:
			LOGMASKED(LOG_WRITE, "%s: %02x: draw mode %06x\n", machine().describe_context(), m_packet.n_entry[ 0 ] >> 24,
				m_packet.n_entry[ 0 ] & 0xffffff );
			decode_tpage( m_packet.n_entry[ 0 ] & 0xffffff );
			break;
		case 0xe2:
			n_twy = ( ( ( m_packet.n_entry[ 0 ] >> 15 ) & 0x1f ) << 3 );
			n_twx = ( ( ( m_packet.n_entry[ 0 ] >> 10 ) & 0x1f ) << 3 );
			n_twh = 255 - ( ( ( m_packet.n_entry[ 0 ] >> 5 ) & 0x1f ) << 3 );
			n_tww = 255 - ( ( m_packet.n_entry[ 0 ] & 0x1f ) << 3 );
			LOGMASKED(LOG_WRITE, "%s: %02x: texture window %u,%u %u,%u\n", machine().describe_context(), m_packet.n_entry[ 0 ] >> 24,
				n_twx, n_twy, n_tww, n_twh );
			break;
		case 0xe3:
			n_drawarea_x1 = m_packet.n_entry[ 0 ] & 1023;
			if( m_n_gputype == 2 )
			{
				n_drawarea_y1 = ( m_packet.n_entry[ 0 ] >> 10 ) & 1023;
			}
			else
			{
				n_drawarea_y1 = ( m_packet.n_entry[ 0 ] >> 12 ) & 1023;
			}
			LOGMASKED(LOG_WRITE, "%s: %02x: drawing area top left %d,%d\n", machine().describe_context(), m_packet.n_entry[ 0 ] >> 24,
				n_drawarea_x1, n_drawarea_y1 );
			break;
		case 0xe4:
			n_drawarea_x2 = m_packet.n_entry[ 0 ] & 1023;
			if( m_n_gputype == 2 )
			{
				n_drawarea_y2 = ( m_packet.n_entry[ 0 ] >> 10 ) & 1023;
			}
			else
			{
				n_drawarea_y2 = ( m_packet.n_entry[ 0 ] >> 12 ) & 1023;
			}
			LOGMASKED(LOG_WRITE, "%s: %02x: drawing area bottom right %d,%d\n", machine().describe_context(), m_packet.n_entry[ 0 ] >> 24,
				n_drawarea_x2, n_drawarea_y2 );
			break;
		case 0xe5:
			n_drawoffset_x = util::sext( m_packet.n_entry[ 0 ] & 2047, 11 );
			if( m_n_gputype == 2 )
			{
				n_drawoffset_y = util::sext( ( m_packet.n_entry[ 0 ] >> 11 ) & 2047, 11 );
			}
			else
			{
				n_drawoffset_y = util::sext( ( m_packet.n_entry[ 0 ] >> 12 ) & 2047, 11 );
			}
			LOGMASKED(LOG_WRITE, "%s: %02x: drawing offset %d,%d\n", machine().describe_context(), m_packet.n_entry[ 0 ] >> 24,
				n_drawoffset_x, n_drawoffset_y );
			break;
		case 0xe6:
			m_draw_stp = BIT( data, 0 );
			m_check_stp = BIT( data, 1 );
			// TODO: confirm status bits on real type 1 gpu
			n_gpustatus &= ~( 3L << 0xb );
			n_gpustatus |= ( data & 0x03 ) << 0xb;
			LOGMASKED(LOG_WRITE, "%s: mask setting %d\n", machine().describe_context(), m_packet.n_entry[ 0 ] & 3);
			break;
		default:
#if defined( MAME_DEBUG )
			popmessage( "unknown GPU packet %08x", m_packet.n_entry[ 0 ] );
#endif
			logerror("%s: unknown GPU packet %08x (%08x)\n", machine().describe_context(), m_packet.n_entry[ 0 ], data);
#if ( STOP_ON_ERROR )
			n_gpu_buffer_offset = 1;
#endif
			break;
		}
		p_ram++;
		n_size--;
		n_word++;
	}
}

void psxgpu_device::write(offs_t offset, uint32_t data, uint32_t mem_mask)
{
	switch( offset )
	{
	case 0x00:
		gpu_write( &data, 1 );
		break;
	case 0x01:
		switch( data >> 24 )
		{
		case 0x00:
			LOGMASKED(LOG_WRITE, "%s: reset gpu\n", machine().describe_context());
			gpu_reset();
			break;
		case 0x01:
			LOGMASKED(LOG_WRITE, "%s: not handled: reset command buffer\n", machine().describe_context());
			n_gpu_buffer_offset = 0;
			break;
		case 0x02:
			LOGMASKED(LOG_WRITE, "%s: not handled: reset irq\n", machine().describe_context());
			break;
		case 0x03:
			n_gpustatus &= ~( 1L << 0x17 );
			n_gpustatus |= ( data & 0x01 ) << 0x17;
			break;
		case 0x04:
			LOGMASKED(LOG_WRITE, "%s: dma setup %d\n", machine().describe_context(), data & 3);
			n_gpustatus &= ~( 3L << 0x1d );
			n_gpustatus |= ( data & 0x03 ) << 0x1d;
			n_gpustatus &= ~( 1L << 0x19 );
			if( ( data & 3 ) == 1 || ( data & 3 ) == 2 )
			{
				n_gpustatus |= ( 1L << 0x19 );
			}
			break;
		case 0x05:
			m_n_displaystartx = data & 1023;
			if( m_n_gputype == 2 )
			{
				n_displaystarty = ( data >> 10 ) & 1023;
			}
			else
			{
				n_displaystarty = ( data >> 12 ) & 1023;
			}
			LOGMASKED(LOG_WRITE, "%s: start of display area %d %d\n", machine().describe_context(), m_n_displaystartx, n_displaystarty);
			break;
		case 0x06:
			n_horiz_disstart = data & 4095;
			n_horiz_disend = ( data >> 12 ) & 4095;
			LOGMASKED(LOG_WRITE, "%s: horizontal display range %d %d\n", machine().describe_context(), n_horiz_disstart, n_horiz_disend);
			break;
		case 0x07:
			n_vert_disstart = data & 1023;
			n_vert_disend = ( data >> 10 ) & 2047;
			LOGMASKED(LOG_WRITE, "%s: vertical display range %d %d\n", machine().describe_context(), n_vert_disstart, n_vert_disend);
			break;
		case 0x08:
			LOGMASKED(LOG_WRITE, "%s: display mode %02x\n", machine().describe_context(), data & 0xff);
			n_gpustatus &= ~( 127L << 0x10 );
			n_gpustatus |= ( data & 0x3f ) << 0x11; /* width 0 + height + videmode + isrgb24 + isinter */
			n_gpustatus |= ( ( data & 0x40 ) >> 0x06 ) << 0x10; /* width 1 */
			if( m_n_gputype == 1 )
			{
				b_reverseflag = ( data >> 7 ) & 1;
			}
			updatevisiblearea();
			break;
		case 0x09:
			LOGMASKED(LOG_WRITE, "%s: not handled: GPU Control 0x09: %08x\n", machine().describe_context(), data);
			break;
		case 0x0d:
			LOGMASKED(LOG_WRITE, "%s: reset lightgun coordinates %08x\n", machine().describe_context(), data);
			n_lightgun_x = 0;
			n_lightgun_y = 0;
			break;
		case 0x10:
			switch( data & 0xff )
			{
			case 0x03:
				if( m_n_gputype == 2 )
				{
					n_gpuinfo = n_drawarea_x1 | ( n_drawarea_y1 << 10 );
				}
				else
				{
					n_gpuinfo = n_drawarea_x1 | ( n_drawarea_y1 << 12 );
				}
				LOGMASKED(LOG_WRITE, "%s: GPU Info - Draw area top left %08x\n", machine().describe_context(), n_gpuinfo);
				break;
			case 0x04:
				if( m_n_gputype == 2 )
				{
					n_gpuinfo = n_drawarea_x2 | ( n_drawarea_y2 << 10 );
				}
				else
				{
					n_gpuinfo = n_drawarea_x2 | ( n_drawarea_y2 << 12 );
				}
				LOGMASKED(LOG_WRITE, "%s: GPU Info - Draw area bottom right %08x\n", machine().describe_context(), n_gpuinfo);
				break;
			case 0x05:
				if( m_n_gputype == 2 )
				{
					n_gpuinfo = ( n_drawoffset_x & 2047 ) | ( ( n_drawoffset_y & 2047 ) << 11 );
				}
				else
				{
					n_gpuinfo = ( n_drawoffset_x & 2047 ) | ( ( n_drawoffset_y & 2047 ) << 12 );
				}
				LOGMASKED(LOG_WRITE, "%s: GPU Info - Draw offset %08x\n", machine().describe_context(), n_gpuinfo);
				break;
			case 0x07:
				n_gpuinfo = m_n_gputype;
				LOGMASKED(LOG_WRITE, "%s: GPU Info - GPU Type %08x\n", machine().describe_context(), n_gpuinfo);
				break;
			case 0x08:
				n_gpuinfo = n_lightgun_x | ( n_lightgun_y << 16 );
				LOGMASKED(LOG_WRITE, "%s: GPU Info - lightgun coordinates %08x\n", machine().describe_context(), n_gpuinfo);
				break;
			default:
				logerror("%s: GPU Info - unknown request (%08x)\n", machine().describe_context(), data);
				n_gpuinfo = 0;
				break;
			}
			break;
		case 0x20:
			LOGMASKED(LOG_WRITE, "%s: not handled: GPU Control 0x20: %08x\n", machine().describe_context(), data);
			break;
		default:
#if defined( MAME_DEBUG )
			popmessage( "unknown GPU command %08x", data );
#endif
			logerror("%s: gpu_w( %08x ) unknown GPU command\n", machine().describe_context(), data);
			break;
		}
		break;
	default:
		logerror("%s: gpu_w( %08x, %08x, %08x ) unknown register\n", machine().describe_context(), offset, data, mem_mask);
		break;
	}
}


void psxgpu_device::dma_read( uint32_t *p_n_psxram, uint32_t n_address, int32_t n_size )
{
	gpu_read( &p_n_psxram[ n_address / 4 ], n_size );
}

void psxgpu_device::gpu_read( uint32_t *p_ram, int32_t n_size )
{
	while( n_size > 0 )
	{
		if( ( n_gpustatus & ( 1L << 0x1b ) ) != 0 )
		{
			PAIR data;

			LOGMASKED(LOG_READ, "%s: copy image from frame buffer ( %d, %d )\n", machine().describe_context(), n_vramx, n_vramy);
			data.d = 0;
			for( int n_pixel = 0; n_pixel < 2; n_pixel++ )
			{
				data.w.l = data.w.h;
				data.w.h = *( p_p_vram[ ( n_vramy + ( m_packet.n_entry[ 1 ] >> 16 ) ) & 0x3ff ] + ( ( n_vramx + ( m_packet.n_entry[ 1 ] & 0xffff ) ) & 0x3ff ) );
				n_vramx++;
				if( n_vramx >= ( m_packet.n_entry[ 2 ] & 0xffff ) )
				{
					n_vramx = 0;
					n_vramy++;
					if( n_vramy >= ( m_packet.n_entry[ 2 ] >> 16 ) )
					{
						LOGMASKED(LOG_READ, "%s: copy image from frame buffer end\n", machine().describe_context());
						n_gpustatus &= ~( 1L << 0x1b );
						n_gpu_buffer_offset = 0;
						n_vramx = 0;
						n_vramy = 0;
						if( n_pixel == 0 )
						{
							data.w.l = data.w.h;
							data.w.h = 0;
						}
						break;
					}
				}
			}
			*( p_ram ) = data.d;
		}
		else
		{
			LOGMASKED(LOG_READ, "%s: read GPU info (%08x)\n", machine().describe_context(), n_gpuinfo);
			*( p_ram ) = n_gpuinfo;
		}
		p_ram++;
		n_size--;
	}
}

uint32_t psxgpu_device::read(offs_t offset, uint32_t mem_mask)
{
	uint32_t data;

	switch( offset )
	{
	case 0x00:
		gpu_read( &data, 1 );
		break;
	case 0x01:
		data = n_gpustatus;

		if ((((n_gpustatus & (1U << 22)) && (n_gpustatus & (1U << 13))) ||
			(!(n_gpustatus & (1U << 22)) && (BIT(screen().vpos(), 0)))))
			data |= 1U << 31;

		LOGMASKED(LOG_READ, "%s: read GPU status (%08x)\n", machine().describe_context(), data);
		break;
	default:
		logerror("%s: gpu_r( %08x, %08x ) unknown register\n", machine().describe_context(), offset, mem_mask);
		data = 0;
		break;
	}
	return data;
}

void psxgpu_device::vblank(screen_device &screen, bool vblank_state)
{
	if( vblank_state )
	{
#if PSXGPU_DEBUG_VIEWER
		DebugCheckKeys();
#endif

		if (n_gpustatus & (1U << 22))
			n_gpustatus ^= 1U << 13;
		else
			n_gpustatus |= 1U << 13;

		m_vblank_handler(1);
	}
}

void psxgpu_device::gpu_reset()
{
	n_gpu_buffer_offset = 0;
	n_gpustatus = 0x14802000;
	n_drawarea_x1 = 0;
	n_drawarea_y1 = 0;
	n_drawarea_x2 = 1023;
	n_drawarea_y2 = 1023;
	n_drawoffset_x = 0;
	n_drawoffset_y = 0;
	m_n_displaystartx = 0;
	n_displaystarty = 0;
	n_horiz_disstart = 0x260;
	n_horiz_disend = 0xc60;
	n_vert_disstart = 0x010;
	n_vert_disend = 0x100;
	n_vramx = 0;
	n_vramy = 0;
	n_twx = 0;
	n_twy = 0;
	n_twh = 255;
	n_tww = 255;
	m_draw_stp = false;
	m_check_stp = false;
	updatevisiblearea();
}

void psxgpu_device::lightgun_set( int n_x, int n_y )
{
	n_lightgun_x = n_x;
	n_lightgun_y = n_y;
}

//-------------------------------------------------
//  device_config_complete - perform any
//  operations now that the configuration is
//  complete
//-------------------------------------------------

void psxgpu_device::device_config_complete()
{
	if (!has_screen())
		return;

	if (!screen().has_been_setup())
	{
		screen().set_refresh_hz(60);
		screen().set_vblank_time(ATTOSECONDS_IN_USEC(2500) /* not accurate */);
		screen().set_size(1024, 1024);
		screen().set_visarea(0, 639, 0, 479);
	}

	if (!screen().has_screen_update())
		screen().set_screen_update(*this, FUNC(psxgpu_device::update_screen));
}
