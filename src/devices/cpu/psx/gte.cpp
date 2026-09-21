// license:BSD-3-Clause
// copyright-holders:smf
/*
 * PlayStation Geometry Transformation Engine emulator
 *
 * Copyright 2003-2013 smf
 *
 */

#include "emu.h"
#include "gte.h"

#include <bit>

#if 0
#include <cstdarg>

void ATTR_PRINTF(2,3) GTELOG( uint32_t pc, const char *a, ...)
{
	va_list va;
	char s_text[ 1024 ];
	va_start( va, a );
	vsprintf( s_text, a, va );
	va_end( va );
	logerror( "%08x: GTE: %s\n", pc, s_text );
}
#else
static inline void ATTR_PRINTF(2,3) GTELOG( uint32_t pc, const char *a, ...) {}
#endif


#define VX0  ( m_cp2dr[ 0 ].sw.l )
#define VY0  ( m_cp2dr[ 0 ].sw.h )
#define VZ0  ( m_cp2dr[ 1 ].sw.l )
#define VX1  ( m_cp2dr[ 2 ].w.l )
#define VY1  ( m_cp2dr[ 2 ].w.h )
#define VZ1  ( m_cp2dr[ 3 ].w.l )
#define VX2  ( m_cp2dr[ 4 ].w.l )
#define VY2  ( m_cp2dr[ 4 ].w.h )
#define VZ2  ( m_cp2dr[ 5 ].w.l )
#define R    ( m_cp2dr[ 6 ].b.l )
#define G    ( m_cp2dr[ 6 ].b.h )
#define B    ( m_cp2dr[ 6 ].b.h2 )
#define CODE ( m_cp2dr[ 6 ].b.h3 )
#define OTZ  ( m_cp2dr[ 7 ].w.l )
#define IR0  ( m_cp2dr[ 8 ].sw.l )
#define IR1  ( m_cp2dr[ 9 ].sw.l )
#define IR2  ( m_cp2dr[ 10 ].sw.l )
#define IR3  ( m_cp2dr[ 11 ].sw.l )
#define SXY0 ( m_cp2dr[ 12 ].d )
#define SX0  ( m_cp2dr[ 12 ].sw.l )
#define SY0  ( m_cp2dr[ 12 ].sw.h )
#define SXY1 ( m_cp2dr[ 13 ].d )
#define SX1  ( m_cp2dr[ 13 ].sw.l )
#define SY1  ( m_cp2dr[ 13 ].sw.h )
#define SXY2 ( m_cp2dr[ 14 ].d )
#define SX2  ( m_cp2dr[ 14 ].sw.l )
#define SY2  ( m_cp2dr[ 14 ].sw.h )
#define SXYP ( m_cp2dr[ 15 ].d )
#define SXP  ( m_cp2dr[ 15 ].sw.l )
#define SYP  ( m_cp2dr[ 15 ].sw.h )
#define SZ0  ( m_cp2dr[ 16 ].w.l )
#define SZ1  ( m_cp2dr[ 17 ].w.l )
#define SZ2  ( m_cp2dr[ 18 ].w.l )
#define SZ3  ( m_cp2dr[ 19 ].w.l )
#define RGB0 ( m_cp2dr[ 20 ].d )
#define R0   ( m_cp2dr[ 20 ].b.l )
#define G0   ( m_cp2dr[ 20 ].b.h )
#define B0   ( m_cp2dr[ 20 ].b.h2 )
#define CD0  ( m_cp2dr[ 20 ].b.h3 )
#define RGB1 ( m_cp2dr[ 21 ].d )
#define R1   ( m_cp2dr[ 21 ].b.l )
#define G1   ( m_cp2dr[ 21 ].b.h )
#define B1   ( m_cp2dr[ 21 ].b.h2 )
#define CD1  ( m_cp2dr[ 21 ].b.h3 )
#define RGB2 ( m_cp2dr[ 22 ].d )
#define R2   ( m_cp2dr[ 22 ].b.l )
#define G2   ( m_cp2dr[ 22 ].b.h )
#define B2   ( m_cp2dr[ 22 ].b.h2 )
#define CD2  ( m_cp2dr[ 22 ].b.h3 )
#define RES1 ( m_cp2dr[ 23 ].d )
#define MAC0 ( m_cp2dr[ 24 ].sd )
#define MAC1 ( m_cp2dr[ 25 ].sd )
#define MAC2 ( m_cp2dr[ 26 ].sd )
#define MAC3 ( m_cp2dr[ 27 ].sd )
#define IRGB ( m_cp2dr[ 28 ].d )
#define ORGB ( m_cp2dr[ 29 ].d )
#define LZCS ( m_cp2dr[ 30 ].d )
#define LZCR ( m_cp2dr[ 31 ].d )

#define R11 ( m_cp2cr[ 0 ].sw.l )
#define R12 ( m_cp2cr[ 0 ].sw.h )
#define R13 ( m_cp2cr[ 1 ].sw.l )
#define R21 ( m_cp2cr[ 1 ].sw.h )
#define R22 ( m_cp2cr[ 2 ].sw.l )
#define R23 ( m_cp2cr[ 2 ].sw.h )
#define R31 ( m_cp2cr[ 3 ].sw.l )
#define R32 ( m_cp2cr[ 3 ].sw.h )
#define R33 ( m_cp2cr[ 4 ].sw.l )
#define TRX ( m_cp2cr[ 5 ].sd )
#define TRY ( m_cp2cr[ 6 ].sd )
#define TRZ ( m_cp2cr[ 7 ].sd )
#define L11 ( m_cp2cr[ 8 ].sw.l )
#define L12 ( m_cp2cr[ 8 ].sw.h )
#define L13 ( m_cp2cr[ 9 ].sw.l )
#define L21 ( m_cp2cr[ 9 ].sw.h )
#define L22 ( m_cp2cr[ 10 ].sw.l )
#define L23 ( m_cp2cr[ 10 ].sw.h )
#define L31 ( m_cp2cr[ 11 ].sw.l )
#define L32 ( m_cp2cr[ 11 ].sw.h )
#define L33 ( m_cp2cr[ 12 ].sw.l )
#define RBK ( m_cp2cr[ 13 ].sd )
#define GBK ( m_cp2cr[ 14 ].sd )
#define BBK ( m_cp2cr[ 15 ].sd )
#define LR1 ( m_cp2cr[ 16 ].sw.l )
#define LR2 ( m_cp2cr[ 16 ].sw.h )
#define LR3 ( m_cp2cr[ 17 ].sw.l )
#define LG1 ( m_cp2cr[ 17 ].sw.h )
#define LG2 ( m_cp2cr[ 18 ].sw.l )
#define LG3 ( m_cp2cr[ 18 ].sw.h )
#define LB1 ( m_cp2cr[ 19 ].sw.l )
#define LB2 ( m_cp2cr[ 19 ].sw.h )
#define LB3 ( m_cp2cr[ 20 ].sw.l )
#define RFC ( m_cp2cr[ 21 ].sd )
#define GFC ( m_cp2cr[ 22 ].sd )
#define BFC ( m_cp2cr[ 23 ].sd )
#define OFX ( m_cp2cr[ 24 ].sd )
#define OFY ( m_cp2cr[ 25 ].sd )
#define H   ( m_cp2cr[ 26 ].sw.l )
#define DQA ( m_cp2cr[ 27 ].sw.l )
#define DQB ( m_cp2cr[ 28 ].sd )
#define ZSF3 ( m_cp2cr[ 29 ].sw.l )
#define ZSF4 ( m_cp2cr[ 30 ].sw.l )
#define FLAG ( m_cp2cr[ 31 ].d )

#define VX( n ) ( n < 3 ? m_cp2dr[ n << 1 ].sw.l : IR1 )
#define VY( n ) ( n < 3 ? m_cp2dr[ n << 1 ].sw.h : IR2 )
#define VZ( n ) ( n < 3 ? m_cp2dr[ ( n << 1 ) + 1 ].sw.l : IR3 )
#define MX11( n ) ( n < 3 ? m_cp2cr[ ( n << 3 ) ].sw.l : -R << 4 )
#define MX12( n ) ( n < 3 ? m_cp2cr[ ( n << 3 ) ].sw.h : R << 4 )
#define MX13( n ) ( n < 3 ? m_cp2cr[ ( n << 3 ) + 1 ].sw.l : IR0 )
#define MX21( n ) ( n < 3 ? m_cp2cr[ ( n << 3 ) + 1 ].sw.h : R13 )
#define MX22( n ) ( n < 3 ? m_cp2cr[ ( n << 3 ) + 2 ].sw.l : R13 )
#define MX23( n ) ( n < 3 ? m_cp2cr[ ( n << 3 ) + 2 ].sw.h : R13 )
#define MX31( n ) ( n < 3 ? m_cp2cr[ ( n << 3 ) + 3 ].sw.l : R22 )
#define MX32( n ) ( n < 3 ? m_cp2cr[ ( n << 3 ) + 3 ].sw.h : R22 )
#define MX33( n ) ( n < 3 ? m_cp2cr[ ( n << 3 ) + 4 ].sw.l : R22 )
#define CV1( n ) ( n < 3 ? m_cp2cr[ ( n << 3 ) + 5 ].sd : 0 )
#define CV2( n ) ( n < 3 ? m_cp2cr[ ( n << 3 ) + 6 ].sd : 0 )
#define CV3( n ) ( n < 3 ? m_cp2cr[ ( n << 3 ) + 7 ].sd : 0 )

int32_t gte::LIM( int32_t value, int32_t max, int32_t min, uint32_t flag )
{
	if( value > max )
	{
		FLAG |= flag;
		return max;
	}
	else if( value < min )
	{
		FLAG |= flag;
		return min;
	}
	return value;
}

uint32_t gte::getcp2dr( uint32_t pc, int reg )
{
	switch( reg )
	{
	case 1:
	case 3:
	case 5:
	case 8:
	case 9:
	case 10:
	case 11:
		m_cp2dr[ reg ].d = (int32_t)m_cp2dr[ reg ].sw.l;
		break;

	case 7:
	case 16:
	case 17:
	case 18:
	case 19:
		m_cp2dr[ reg ].d = (uint32_t)m_cp2dr[ reg ].w.l;
		break;

	case 15:
		m_cp2dr[ reg ].d = SXY2;
		break;

	case 28:
	case 29:
		m_cp2dr[ reg ].d = LIM( IR1 >> 7, 0x1f, 0, 0 ) | ( LIM( IR2 >> 7, 0x1f, 0, 0 ) << 5 ) | ( LIM( IR3 >> 7, 0x1f, 0, 0 ) << 10 );
		break;
	}

	GTELOG( pc, "get CP2DR%u=%08x", reg, m_cp2dr[ reg ].d );
	return m_cp2dr[ reg ].d;
}

void gte::setcp2dr( uint32_t pc, int reg, uint32_t value )
{
	GTELOG( pc, "set CP2DR%u=%08x", reg, value );

	switch( reg )
	{
	case 15:
	{
		// PGXP (CLAUDE.md Phase 4c/investigation): MTC2/CTC2 writing SXYP
		// pushes a value into the FIFO without going through RTPS/RTPT's
		// own shadow write - but the overwhelmingly common real use of
		// this isn't an arbitrary unrelated coordinate, it's PS1 code
		// re-sharing an already-computed vertex across a triangle
		// fan/strip: read one of SXY0/SXY1/SXY2 back via MFC2, then push
		// that *same* value into SXYP to advance the FIFO without
		// recomputing it (found live: this is the dominant pattern on
		// subdivided ground/floor geometry, which leans on fan/strip
		// vertex sharing far more than character models do - blanket
		// invalidation here was killing the shadow for nearly every
		// ground-plane vertex, even though nothing about the underlying
		// float position was actually unknown). So: only invalidate when
		// `value` doesn't match any of the three real SXY registers as
		// they stood *before* this shift - a genuine match reuses that
		// slot's already-computed shadow instead of losing it.
		uint32_t old_sxy0 = SXY0;
		uint32_t old_sxy1 = SXY1;
		uint32_t old_sxy2 = SXY2;
		pgxp_shadow old_shadow0 = m_pgxp_shadow[ 0 ];
		pgxp_shadow old_shadow1 = m_pgxp_shadow[ 1 ];
		pgxp_shadow old_shadow2 = m_pgxp_shadow[ 2 ];

		SXY0 = old_sxy1;
		SXY1 = old_sxy2;
		SXY2 = value;

		m_pgxp_shadow[ 0 ] = old_shadow1;
		m_pgxp_shadow[ 1 ] = old_shadow2;
		if( value == old_sxy2 )
			m_pgxp_shadow[ 2 ] = old_shadow2;
		else if( value == old_sxy1 )
			m_pgxp_shadow[ 2 ] = old_shadow1;
		else if( value == old_sxy0 )
			m_pgxp_shadow[ 2 ] = old_shadow0;
		else
			m_pgxp_shadow[ 2 ] = pgxp_shadow();
		break;
	}

	case 28:
		IR1 = ( value & 0x1f ) << 7;
		IR2 = ( value & 0x3e0 ) << 2;
		IR3 = ( value & 0x7c00 ) >> 3;
		break;

	case 30:
		LZCR = (value & 0x80000000) == 0 ? std::countl_zero(value) : std::countl_one(value);
		break;

	case 31:
		return;
	}

	m_cp2dr[ reg ].d = value;
}

uint32_t gte::getcp2cr( uint32_t pc, int reg )
{
	GTELOG( pc, "get CP2CR%u=%08x", reg, m_cp2cr[ reg ].d );

	return m_cp2cr[ reg ].d;
}

void gte::setcp2cr( uint32_t pc, int reg, uint32_t value )
{
	GTELOG( pc, "set CP2CR%u=%08x", reg, value );

	switch( reg )
	{
	case 4:
	case 12:
	case 20:
	case 26:
	case 27:
	case 29:
	case 30:
		value = (int32_t)(int16_t) value;
		break;

	case 31:
		value = value & 0x7ffff000;
		if( ( value & 0x7f87e000 ) != 0 )
		{
			value |= 0x80000000;
		}
		break;
	}

	m_cp2cr[ reg ].d = value;
}

static inline int64_t gte_shift( int64_t a, int sf )
{
	if( sf > 0 )
	{
		return a >> 12;
	}
	else if( sf < 0 )
	{
		return a << 12;
	}

	return a;
}

int32_t gte::BOUNDS( int44 value, int max_flag, int min_flag )
{
	if( value.positive_overflow() )
	{
		FLAG |= max_flag;
	}

	if( value.negative_overflow() )
	{
		FLAG |= min_flag;
	}

	return gte_shift( value.value(), m_sf );
}

static inline uint32_t gte_divide( uint16_t numerator, uint16_t denominator )
{
	if( numerator < ( denominator * 2 ) )
	{
		static const uint8_t table[] =
		{
			0xff, 0xfd, 0xfb, 0xf9, 0xf7, 0xf5, 0xf3, 0xf1, 0xef, 0xee, 0xec, 0xea, 0xe8, 0xe6, 0xe4, 0xe3,
			0xe1, 0xdf, 0xdd, 0xdc, 0xda, 0xd8, 0xd6, 0xd5, 0xd3, 0xd1, 0xd0, 0xce, 0xcd, 0xcb, 0xc9, 0xc8,
			0xc6, 0xc5, 0xc3, 0xc1, 0xc0, 0xbe, 0xbd, 0xbb, 0xba, 0xb8, 0xb7, 0xb5, 0xb4, 0xb2, 0xb1, 0xb0,
			0xae, 0xad, 0xab, 0xaa, 0xa9, 0xa7, 0xa6, 0xa4, 0xa3, 0xa2, 0xa0, 0x9f, 0x9e, 0x9c, 0x9b, 0x9a,
			0x99, 0x97, 0x96, 0x95, 0x94, 0x92, 0x91, 0x90, 0x8f, 0x8d, 0x8c, 0x8b, 0x8a, 0x89, 0x87, 0x86,
			0x85, 0x84, 0x83, 0x82, 0x81, 0x7f, 0x7e, 0x7d, 0x7c, 0x7b, 0x7a, 0x79, 0x78, 0x77, 0x75, 0x74,
			0x73, 0x72, 0x71, 0x70, 0x6f, 0x6e, 0x6d, 0x6c, 0x6b, 0x6a, 0x69, 0x68, 0x67, 0x66, 0x65, 0x64,
			0x63, 0x62, 0x61, 0x60, 0x5f, 0x5e, 0x5d, 0x5d, 0x5c, 0x5b, 0x5a, 0x59, 0x58, 0x57, 0x56, 0x55,
			0x54, 0x53, 0x53, 0x52, 0x51, 0x50, 0x4f, 0x4e, 0x4d, 0x4d, 0x4c, 0x4b, 0x4a, 0x49, 0x48, 0x48,
			0x47, 0x46, 0x45, 0x44, 0x43, 0x43, 0x42, 0x41, 0x40, 0x3f, 0x3f, 0x3e, 0x3d, 0x3c, 0x3c, 0x3b,
			0x3a, 0x39, 0x39, 0x38, 0x37, 0x36, 0x36, 0x35, 0x34, 0x33, 0x33, 0x32, 0x31, 0x31, 0x30, 0x2f,
			0x2e, 0x2e, 0x2d, 0x2c, 0x2c, 0x2b, 0x2a, 0x2a, 0x29, 0x28, 0x28, 0x27, 0x26, 0x26, 0x25, 0x24,
			0x24, 0x23, 0x22, 0x22, 0x21, 0x20, 0x20, 0x1f, 0x1e, 0x1e, 0x1d, 0x1d, 0x1c, 0x1b, 0x1b, 0x1a,
			0x19, 0x19, 0x18, 0x18, 0x17, 0x16, 0x16, 0x15, 0x15, 0x14, 0x14, 0x13, 0x12, 0x12, 0x11, 0x11,
			0x10, 0x0f, 0x0f, 0x0e, 0x0e, 0x0d, 0x0d, 0x0c, 0x0c, 0x0b, 0x0a, 0x0a, 0x09, 0x09, 0x08, 0x08,
			0x07, 0x07, 0x06, 0x06, 0x05, 0x05, 0x04, 0x04, 0x03, 0x03, 0x02, 0x02, 0x01, 0x01, 0x00, 0x00,
			0x00
		};

		int shift = std::countl_zero( denominator );

		int r1 = ( denominator << shift ) & 0x7fff;
		int r2 = table[ ( ( r1 + 0x40 ) >> 7 ) ] + 0x101;
		int r3 = ( ( 0x80 - ( r2 * ( r1 + 0x8000 ) ) ) >> 8 ) & 0x1ffff;
		uint32_t reciprocal = ( ( r2 * r3 ) + 0x80 ) >> 8;

		return uint32_t( ( ( uint64_t(reciprocal) * ( numerator << shift ) ) + 0x8000 ) >> 16 );
	}

	return 0xffffffff;
}

/* Setting bits 12 & 19-22 in FLAG does not set bit 31 */

int32_t gte::A1( int44 a ) { m_mac1 = a.value(); return BOUNDS( a, ( 1 << 31 ) | ( 1 << 30 ), ( 1 << 31 ) | ( 1 << 27 ) ); }
int32_t gte::A2( int44 a ) { m_mac2 = a.value(); return BOUNDS( a, ( 1 << 31 ) | ( 1 << 29 ), ( 1 << 31 ) | ( 1 << 26 ) ); }
int32_t gte::A3( int44 a ) { m_mac3 = a.value(); return BOUNDS( a, ( 1 << 31 ) | ( 1 << 28 ), ( 1 << 31 ) | ( 1 << 25 ) ); }
int32_t gte::Lm_B1( int32_t a, int lm ) { return LIM( a, 0x7fff, -0x8000 * !lm, ( 1 << 31 ) | ( 1 << 24 ) ); }
int32_t gte::Lm_B2( int32_t a, int lm ) { return LIM( a, 0x7fff, -0x8000 * !lm, ( 1 << 31 ) | ( 1 << 23 ) ); }
int32_t gte::Lm_B3( int32_t a, int lm ) { return LIM( a, 0x7fff, -0x8000 * !lm, ( 1 << 22 ) ); }

int32_t gte::Lm_B3_sf( int64_t value, int sf, int lm )
{
	int32_t value_sf = gte_shift( value, sf );
	int32_t value_12 = gte_shift( value, 1 );
	int max = 0x7fff;
	int min = 0;
	if( lm == 0 )
	{
		min = -0x8000;
	}

	if( value_12 < -0x8000 || value_12 > 0x7fff )
	{
		FLAG |= ( 1 << 22 );
	}

	if( value_sf > max )
	{
		return max;
	}
	else if( value_sf < min )
	{
		return min;
	}

	return value_sf;
}

int32_t gte::Lm_C1( int32_t a ) { return LIM( a, 0x00ff, 0x0000, ( 1 << 21 ) ); }
int32_t gte::Lm_C2( int32_t a ) { return LIM( a, 0x00ff, 0x0000, ( 1 << 20 ) ); }
int32_t gte::Lm_C3( int32_t a ) { return LIM( a, 0x00ff, 0x0000, ( 1 << 19 ) ); }
int32_t gte::Lm_D( int64_t a, int sf ) { return LIM( gte_shift( a, sf ), 0xffff, 0x0000, ( 1 << 31 ) | ( 1 << 18 ) ); }

uint32_t gte::Lm_E( uint32_t result )
{
	if( result == 0xffffffff )
	{
		FLAG |= ( 1 << 31 ) | ( 1 << 17 );
		return 0x1ffff;
	}

	if( result > 0x1ffff )
	{
		return 0x1ffff;
	}

	return result;
}

// PGXP-style geometry/texture correction (CLAUDE.md Phase 4c). Redoes
// RTPS/RTPT's OFX/OFY + IR1_or_2 * (H/SZ3) perspective-projection step in
// double precision instead of the hardware's F()>>16 fixed-point rounding
// and limited-precision reciprocal-table approximation (gte_divide()), and
// rotates the result into m_pgxp_shadow[] exactly where docop2() rotates
// the real SXY0/SXY1/SXY2 FIFO - see pgxp_shadow_for_reg() for how a
// caller reads it back. Earlier revisions of this (Phase 4b) cached the
// result in a value-keyed hash table instead, keyed by the fixed-point
// SXY word - after several rounds of real bugs in that approach (bad
// hash, unbounded staleness) still left live-tested seams/shimmer, a
// comparison against libretro/beetle-psx-libretro's real PGXP
// implementation confirmed a value-keyed cache is exactly its own
// documented-broken fallback mode, not its recommended mechanism (see
// CLAUDE.md Phase 4c) - replaced with this FIFO-mirror plus end-to-end
// GPR/RAM-address shadow propagation (psxcpu_device/psxgpu_device).
//
// Takes ir1/ir2 (the *clamped* IR1/IR2, not the raw pre-clamp MAC1/MAC2)
// deliberately - using MAC1/MAC2 for "more precision" was tried first and
// was a real bug: A1()/A2() never actually clamp their return value (only
// flag overflow), so for a near-degenerate/behind-camera vertex MAC1/MAC2
// could reach values IR1/IR2's hardware clamp would normally bound -
// multiplied into the perspective formula, that produced screen positions
// literally millions of pixels off. IR1/IR2 match MAC1/MAC2 exactly for
// any vertex within the clamp's range anyway (the vast majority), so this
// costs nothing for the normal case while restoring the same safety
// bound hardware relies on.
//
// Two more hardware clamps this recomputation must reproduce or it
// produces systematically wrong (not just imprecise) results:
//   - h_over_sz3 (gte_divide()+Lm_E()) is capped to 0x1ffff/65536
//     (≈1.9999847) - not a rare case, this triggers for any vertex closer
//     than half the "H" reference distance, i.e. ordinary foreground
//     geometry. A plain double H/SZ3 division runs unboundedly higher
//     than that whenever it applies.
//   - The final x/y is capped to [-1024,1023] (Lm_G1/Lm_G2) - hardware's
//     own safety net for a near-zero SZ3 blowing up the divide regardless
//     of how IR1/IR2 are bounded.
//
// sz3 (the vertex's raw eye-space depth) is stored as-is for
// perspective-correct texture/color interpolation - see
// psxgpu_device::gpu_vertex_xyw() and retro_gpu_target's vertex shader.
// This never changes any existing register/FLAG value.
void gte::pgxp_write_shadow( int64_t ir1, int64_t ir2, int32_t sz3 )
{
	m_pgxp_shadow[ 0 ] = m_pgxp_shadow[ 1 ];
	m_pgxp_shadow[ 1 ] = m_pgxp_shadow[ 2 ];
	m_pgxp_shadow[ 2 ] = pgxp_shadow();

	if( sz3 <= 0 )
	{
		// Degenerate/behind-camera vertex - gte_divide() itself saturates
		// to its max clamp here (see Lm_E()); no meaningful precise value
		// to shadow, leave slot 2 invalid (falls back to the plain
		// integer coordinate, same as any other invalid shadow entry).
		return;
	}

	const double PGXP_H_OVER_SZ3_MAX = 131071.0 / 65536.0;
	double h_over_sz3 = (double) H / (double) sz3;
	if( h_over_sz3 > PGXP_H_OVER_SZ3_MAX )
	{
		h_over_sz3 = PGXP_H_OVER_SZ3_MAX;
	}
	double x = ( (double) OFX / 65536.0 ) + ( (double) ir1 * h_over_sz3 );
	double y = ( (double) OFY / 65536.0 ) + ( (double) ir2 * h_over_sz3 );

	if( x > 1023.0 ) x = 1023.0;
	else if( x < -1024.0 ) x = -1024.0;
	if( y > 1023.0 ) y = 1023.0;
	else if( y < -1024.0 ) y = -1024.0;

	m_pgxp_shadow[ 2 ].x = (float) x;
	m_pgxp_shadow[ 2 ].y = (float) y;
	m_pgxp_shadow[ 2 ].w = (float) sz3;
	m_pgxp_shadow[ 2 ].valid = true;
	// PGXP (CLAUDE.md Phase 4c investigation): tag with SXY2's own real
	// value - SX2/SY2 are already written by the caller above this call
	// (RTPS/RTPT set them immediately before calling pgxp_write_shadow())
	// - so this is the real hardware register content, not a
	// recomputation. Lets a later MFC2 validate this shadow against
	// whatever SXY2 actually holds *at that read*, rather than trusting
	// it purely because the FIFO rotation is expected to stay in sync;
	// see the MFC2 site's comment in psx.cpp for why that expectation
	// alone wasn't a safe enough guarantee in practice.
	m_pgxp_shadow[ 2 ].tag_value = SXY2;

	if( m_pgxp_vcache_enabled )
	{
		pgxp_vcache_write( (int16_t)( SXY2 & 0xffff ), (int16_t)( SXY2 >> 16 ), m_pgxp_shadow[ 2 ].x, m_pgxp_shadow[ 2 ].y, m_pgxp_shadow[ 2 ].w );
	}
}

// Port of Beetle's PGXP_CacheVertex(): the first write after a read opens a
// new session (clearing the previous one - equivalent to Beetle's generation
// counter retiring old entries); a second, different vertex claiming the same
// integer position within a session marks it ambiguous.
void gte::pgxp_vcache_write( int sx, int sy, float x, float y, float w )
{
	if( sx < -2048 || sx > 2047 || sy < -2048 || sy > 2047 )
		return;

	if( !m_pgxp_vcache_writing )
	{
		m_pgxp_vcache.clear();
		m_pgxp_vcache_writing = true;
	}

	auto it = m_pgxp_vcache.find( pgxp_vcache_key( sx, sy ) );
	if( it == m_pgxp_vcache.end() )
	{
		m_pgxp_vcache[ pgxp_vcache_key( sx, sy ) ] = { x, y, w, false };
	}
	else if( it->second.x != x || it->second.y != y || it->second.w != w )
	{
		it->second.ambiguous = true;
	}
}

// Port of Beetle's PGXP_GetCachedVertex(): declines (false) for a position
// never written this session or claimed twice; the caller then falls back to
// the plain native coordinate.
bool gte::pgxp_vertex_cache_query( int sx, int sy, float &x, float &y, float &w )
{
	if( !m_pgxp_vcache_enabled )
		return false;

	m_pgxp_vcache_writing = false;

	if( sx < -2048 || sx > 2047 || sy < -2048 || sy > 2047 )
		return false;

	auto it = m_pgxp_vcache.find( pgxp_vcache_key( sx, sy ) );
	if( it == m_pgxp_vcache.end() || it->second.ambiguous )
		return false;

	x = it->second.x; y = it->second.y; w = it->second.w;
	return true;
}

gte::pgxp_shadow gte::pgxp_shadow_for_reg( int reg ) const
{
	switch( reg )
	{
	case 12: return m_pgxp_shadow[ 0 ];
	case 13: return m_pgxp_shadow[ 1 ];
	case 14:
	case 15: return m_pgxp_shadow[ 2 ]; // SXYP aliases SXY2 - see getcp2dr().
	default: return pgxp_shadow();
	}
}

int64_t gte::F( int64_t a )
{
	m_mac0 = a;

	if( a > 0x7fffffff )
	{
		FLAG |= ( 1 << 31 ) | ( 1 << 16 );
	}

	if( a < (int32_t) -0x80000000 )
	{
		FLAG |= ( 1 << 31 ) | ( 1 << 15 );
	}

	return a;
}

int32_t gte::Lm_G1( int64_t a )
{
	if( a > 0x3ff )
	{
		FLAG |= ( 1 << 31 ) | ( 1 << 14 );
		return 0x3ff;
	}

	if( a < -0x400 )
	{
		FLAG |= ( 1 << 31 ) | ( 1 << 14 );
		return -0x400;
	}

	return a;
}

int32_t gte::Lm_G2( int64_t a )
{
	if( a > 0x3ff )
	{
		FLAG |= ( 1 << 31 ) | ( 1 << 13 );
		return 0x3ff;
	}

	if( a < -0x400 )
	{
		FLAG |= ( 1 << 31 ) | ( 1 << 13 );
		return -0x400;
	}

	return a;
}

int32_t gte::Lm_H( int64_t value, int sf )
{
	int64_t value_sf = gte_shift( value, sf );
	int32_t value_12 = gte_shift( value, 1 );
	int max = 0x1000;
	int min = 0x0000;

	if( value_sf < min || value_sf > max )
	{
		FLAG |= ( 1 << 12 );
	}

	if( value_12 > max )
	{
		return max;
	}

	if( value_12 < min )
	{
		return min;
	}

	return value_12;
}

int gte::docop2( uint32_t pc, int gteop )
{
	int v;
	int lm;
	int cv;
	int mx;
	int32_t h_over_sz3 = 0;

	lm = GTE_LM( gteop );
	m_sf = GTE_SF( gteop );

	FLAG = 0;

	switch( GTE_FUNCT( gteop ) )
	{
	case 0x00: // drop through to RTPS
	case 0x01:
		GTELOG( pc, "%08x RTPS", gteop );

		MAC1 = A1( int44( (int64_t) TRX << 12 ) + ( R11 * VX0 ) + ( R12 * VY0 ) + ( R13 * VZ0 ) );
		MAC2 = A2( int44( (int64_t) TRY << 12 ) + ( R21 * VX0 ) + ( R22 * VY0 ) + ( R23 * VZ0 ) );
		MAC3 = A3( int44( (int64_t) TRZ << 12 ) + ( R31 * VX0 ) + ( R32 * VY0 ) + ( R33 * VZ0 ) );
		IR1 = Lm_B1( MAC1, lm );
		IR2 = Lm_B2( MAC2, lm );
		IR3 = Lm_B3_sf( m_mac3, m_sf, lm );
		SZ0 = SZ1;
		SZ1 = SZ2;
		SZ2 = SZ3;
		SZ3 = Lm_D( m_mac3, 1 );
		h_over_sz3 = Lm_E( gte_divide( H, SZ3 ) );
		SXY0 = SXY1;
		SXY1 = SXY2;
		SX2 = Lm_G1( F( (int64_t) OFX + ( (int64_t) IR1 * h_over_sz3 ) ) >> 16 );
		SY2 = Lm_G2( F( (int64_t) OFY + ( (int64_t) IR2 * h_over_sz3 ) ) >> 16 );
		MAC0 = F( (int64_t) DQB + ( (int64_t) DQA * h_over_sz3 ) );
		IR0 = Lm_H( m_mac0, 1 );
		if( m_pgxp_enabled )
		{
			pgxp_write_shadow( IR1, IR2, SZ3 );
		}
		return 1;

	case 0x06:
		GTELOG( pc, "%08x NCLIP", gteop );

		MAC0 = F( (int64_t) ( SX0 * SY1 ) + ( SX1 * SY2 ) + ( SX2 * SY0 ) - ( SX0 * SY2 ) - ( SX1 * SY0 ) - ( SX2 * SY1 ) );
		return 1;

	case 0x0c:
		GTELOG( pc, "%08x OP", gteop );

		MAC1 = A1( (int64_t) ( R22 * IR3 ) - ( R33 * IR2 ) );
		MAC2 = A2( (int64_t) ( R33 * IR1 ) - ( R11 * IR3 ) );
		MAC3 = A3( (int64_t) ( R11 * IR2 ) - ( R22 * IR1 ) );
		IR1 = Lm_B1( MAC1, lm );
		IR2 = Lm_B2( MAC2, lm );
		IR3 = Lm_B3( MAC3, lm );
		return 1;

	case 0x10:
		GTELOG( pc, "%08x DPCS", gteop );

		MAC1 = A1( ( R << 16 ) + ( IR0 * Lm_B1( A1( ( (int64_t) RFC << 12 ) - ( R << 16 ) ), 0 ) ) );
		MAC2 = A2( ( G << 16 ) + ( IR0 * Lm_B2( A2( ( (int64_t) GFC << 12 ) - ( G << 16 ) ), 0 ) ) );
		MAC3 = A3( ( B << 16 ) + ( IR0 * Lm_B3( A3( ( (int64_t) BFC << 12 ) - ( B << 16 ) ), 0 ) ) );
		IR1 = Lm_B1( MAC1, lm );
		IR2 = Lm_B2( MAC2, lm );
		IR3 = Lm_B3( MAC3, lm );
		RGB0 = RGB1;
		RGB1 = RGB2;
		CD2 = CODE;
		R2 = Lm_C1( MAC1 >> 4 );
		G2 = Lm_C2( MAC2 >> 4 );
		B2 = Lm_C3( MAC3 >> 4 );
		return 1;

	case 0x11:
		GTELOG( pc, "%08x INTPL", gteop );

		MAC1 = A1( ( IR1 << 12 ) + ( IR0 * Lm_B1( A1( ( (int64_t) RFC << 12 ) - ( IR1 << 12 ) ), 0 ) ) );
		MAC2 = A2( ( IR2 << 12 ) + ( IR0 * Lm_B2( A2( ( (int64_t) GFC << 12 ) - ( IR2 << 12 ) ), 0 ) ) );
		MAC3 = A3( ( IR3 << 12 ) + ( IR0 * Lm_B3( A3( ( (int64_t) BFC << 12 ) - ( IR3 << 12 ) ), 0 ) ) );
		IR1 = Lm_B1( MAC1, lm );
		IR2 = Lm_B2( MAC2, lm );
		IR3 = Lm_B3( MAC3, lm );
		RGB0 = RGB1;
		RGB1 = RGB2;
		CD2 = CODE;
		R2 = Lm_C1( MAC1 >> 4 );
		G2 = Lm_C2( MAC2 >> 4 );
		B2 = Lm_C3( MAC3 >> 4 );
		return 1;

	case 0x12:
		GTELOG( pc, "%08x MVMVA", gteop );

		mx = GTE_MX( gteop );
		v = GTE_V( gteop );
		cv = GTE_CV( gteop );

		switch( cv )
		{
		case 2:
			MAC1 = A1( (int64_t) ( MX12( mx ) * VY( v ) ) + ( MX13( mx ) * VZ( v ) ) );
			MAC2 = A2( (int64_t) ( MX22( mx ) * VY( v ) ) + ( MX23( mx ) * VZ( v ) ) );
			MAC3 = A3( (int64_t) ( MX32( mx ) * VY( v ) ) + ( MX33( mx ) * VZ( v ) ) );
			Lm_B1( A1( ( (int64_t) CV1( cv ) << 12 ) + ( MX11( mx ) * VX( v ) ) ), 0 );
			Lm_B2( A2( ( (int64_t) CV2( cv ) << 12 ) + ( MX21( mx ) * VX( v ) ) ), 0 );
			Lm_B3( A3( ( (int64_t) CV3( cv ) << 12 ) + ( MX31( mx ) * VX( v ) ) ), 0 );
			break;

		default:
			MAC1 = A1( int44( (int64_t) CV1( cv ) << 12 ) + ( MX11( mx ) * VX( v ) ) + ( MX12( mx ) * VY( v ) ) + ( MX13( mx ) * VZ( v ) ) );
			MAC2 = A2( int44( (int64_t) CV2( cv ) << 12 ) + ( MX21( mx ) * VX( v ) ) + ( MX22( mx ) * VY( v ) ) + ( MX23( mx ) * VZ( v ) ) );
			MAC3 = A3( int44( (int64_t) CV3( cv ) << 12 ) + ( MX31( mx ) * VX( v ) ) + ( MX32( mx ) * VY( v ) ) + ( MX33( mx ) * VZ( v ) ) );
			break;
		}

		IR1 = Lm_B1( MAC1, lm );
		IR2 = Lm_B2( MAC2, lm );
		IR3 = Lm_B3( MAC3, lm );
		return 1;

	case 0x13:
		GTELOG( pc, "%08x NCDS", gteop );

		MAC1 = A1( (int64_t) ( L11 * VX0 ) + ( L12 * VY0 ) + ( L13 * VZ0 ) );
		MAC2 = A2( (int64_t) ( L21 * VX0 ) + ( L22 * VY0 ) + ( L23 * VZ0 ) );
		MAC3 = A3( (int64_t) ( L31 * VX0 ) + ( L32 * VY0 ) + ( L33 * VZ0 ) );
		IR1 = Lm_B1( MAC1, lm );
		IR2 = Lm_B2( MAC2, lm );
		IR3 = Lm_B3( MAC3, lm );
		MAC1 = A1( int44( (int64_t) RBK << 12 ) + ( LR1 * IR1 ) + ( LR2 * IR2 ) + ( LR3 * IR3 ) );
		MAC2 = A2( int44( (int64_t) GBK << 12 ) + ( LG1 * IR1 ) + ( LG2 * IR2 ) + ( LG3 * IR3 ) );
		MAC3 = A3( int44( (int64_t) BBK << 12 ) + ( LB1 * IR1 ) + ( LB2 * IR2 ) + ( LB3 * IR3 ) );
		IR1 = Lm_B1( MAC1, lm );
		IR2 = Lm_B2( MAC2, lm );
		IR3 = Lm_B3( MAC3, lm );
		MAC1 = A1( ( ( R << 4 ) * IR1 ) + ( IR0 * Lm_B1( A1( ( (int64_t) RFC << 12 ) - ( ( R << 4 ) * IR1 ) ), 0 ) ) );
		MAC2 = A2( ( ( G << 4 ) * IR2 ) + ( IR0 * Lm_B2( A2( ( (int64_t) GFC << 12 ) - ( ( G << 4 ) * IR2 ) ), 0 ) ) );
		MAC3 = A3( ( ( B << 4 ) * IR3 ) + ( IR0 * Lm_B3( A3( ( (int64_t) BFC << 12 ) - ( ( B << 4 ) * IR3 ) ), 0 ) ) );
		IR1 = Lm_B1( MAC1, lm );
		IR2 = Lm_B2( MAC2, lm );
		IR3 = Lm_B3( MAC3, lm );
		RGB0 = RGB1;
		RGB1 = RGB2;
		CD2 = CODE;
		R2 = Lm_C1( MAC1 >> 4 );
		G2 = Lm_C2( MAC2 >> 4 );
		B2 = Lm_C3( MAC3 >> 4 );
		return 1;

	case 0x14:
		GTELOG( pc, "%08x CDP", gteop );

		MAC1 = A1( int44( (int64_t) RBK << 12 ) + ( LR1 * IR1 ) + ( LR2 * IR2 ) + ( LR3 * IR3 ) );
		MAC2 = A2( int44( (int64_t) GBK << 12 ) + ( LG1 * IR1 ) + ( LG2 * IR2 ) + ( LG3 * IR3 ) );
		MAC3 = A3( int44( (int64_t) BBK << 12 ) + ( LB1 * IR1 ) + ( LB2 * IR2 ) + ( LB3 * IR3 ) );
		IR1 = Lm_B1( MAC1, lm );
		IR2 = Lm_B2( MAC2, lm );
		IR3 = Lm_B3( MAC3, lm );
		MAC1 = A1( ( ( R << 4 ) * IR1 ) + ( IR0 * Lm_B1( A1( ( (int64_t) RFC << 12 ) - ( ( R << 4 ) * IR1 ) ), 0 ) ) );
		MAC2 = A2( ( ( G << 4 ) * IR2 ) + ( IR0 * Lm_B2( A2( ( (int64_t) GFC << 12 ) - ( ( G << 4 ) * IR2 ) ), 0 ) ) );
		MAC3 = A3( ( ( B << 4 ) * IR3 ) + ( IR0 * Lm_B3( A3( ( (int64_t) BFC << 12 ) - ( ( B << 4 ) * IR3 ) ), 0 ) ) );
		IR1 = Lm_B1( MAC1, lm );
		IR2 = Lm_B2( MAC2, lm );
		IR3 = Lm_B3( MAC3, lm );
		RGB0 = RGB1;
		RGB1 = RGB2;
		CD2 = CODE;
		R2 = Lm_C1( MAC1 >> 4 );
		G2 = Lm_C2( MAC2 >> 4 );
		B2 = Lm_C3( MAC3 >> 4 );
		return 1;

	case 0x16:
		GTELOG( pc, "%08x NCDT", gteop );

		for( v = 0; v < 3; v++ )
		{
			MAC1 = A1( (int64_t) ( L11 * VX( v ) ) + ( L12 * VY( v ) ) + ( L13 * VZ( v ) ) );
			MAC2 = A2( (int64_t) ( L21 * VX( v ) ) + ( L22 * VY( v ) ) + ( L23 * VZ( v ) ) );
			MAC3 = A3( (int64_t) ( L31 * VX( v ) ) + ( L32 * VY( v ) ) + ( L33 * VZ( v ) ) );
			IR1 = Lm_B1( MAC1, lm );
			IR2 = Lm_B2( MAC2, lm );
			IR3 = Lm_B3( MAC3, lm );
			MAC1 = A1( int44( (int64_t) RBK << 12 ) + ( LR1 * IR1 ) + ( LR2 * IR2 ) + ( LR3 * IR3 ) );
			MAC2 = A2( int44( (int64_t) GBK << 12 ) + ( LG1 * IR1 ) + ( LG2 * IR2 ) + ( LG3 * IR3 ) );
			MAC3 = A3( int44( (int64_t) BBK << 12 ) + ( LB1 * IR1 ) + ( LB2 * IR2 ) + ( LB3 * IR3 ) );
			IR1 = Lm_B1( MAC1, lm );
			IR2 = Lm_B2( MAC2, lm );
			IR3 = Lm_B3( MAC3, lm );
			MAC1 = A1( ( ( R << 4 ) * IR1 ) + ( IR0 * Lm_B1( A1( ( (int64_t) RFC << 12 ) - ( ( R << 4 ) * IR1 ) ), 0 ) ) );
			MAC2 = A2( ( ( G << 4 ) * IR2 ) + ( IR0 * Lm_B2( A2( ( (int64_t) GFC << 12 ) - ( ( G << 4 ) * IR2 ) ), 0 ) ) );
			MAC3 = A3( ( ( B << 4 ) * IR3 ) + ( IR0 * Lm_B3( A3( ( (int64_t) BFC << 12 ) - ( ( B << 4 ) * IR3 ) ), 0 ) ) );
			IR1 = Lm_B1( MAC1, lm );
			IR2 = Lm_B2( MAC2, lm );
			IR3 = Lm_B3( MAC3, lm );
			RGB0 = RGB1;
			RGB1 = RGB2;
			CD2 = CODE;
			R2 = Lm_C1( MAC1 >> 4 );
			G2 = Lm_C2( MAC2 >> 4 );
			B2 = Lm_C3( MAC3 >> 4 );
		}
		return 1;

	case 0x1b:
		GTELOG( pc, "%08x NCCS", gteop );

		MAC1 = A1( (int64_t) ( L11 * VX0 ) + ( L12 * VY0 ) + ( L13 * VZ0 ) );
		MAC2 = A2( (int64_t) ( L21 * VX0 ) + ( L22 * VY0 ) + ( L23 * VZ0 ) );
		MAC3 = A3( (int64_t) ( L31 * VX0 ) + ( L32 * VY0 ) + ( L33 * VZ0 ) );
		IR1 = Lm_B1( MAC1, lm );
		IR2 = Lm_B2( MAC2, lm );
		IR3 = Lm_B3( MAC3, lm );
		MAC1 = A1( int44( (int64_t) RBK << 12 ) + ( LR1 * IR1 ) + ( LR2 * IR2 ) + ( LR3 * IR3 ) );
		MAC2 = A2( int44( (int64_t) GBK << 12 ) + ( LG1 * IR1 ) + ( LG2 * IR2 ) + ( LG3 * IR3 ) );
		MAC3 = A3( int44( (int64_t) BBK << 12 ) + ( LB1 * IR1 ) + ( LB2 * IR2 ) + ( LB3 * IR3 ) );
		IR1 = Lm_B1( MAC1, lm );
		IR2 = Lm_B2( MAC2, lm );
		IR3 = Lm_B3( MAC3, lm );
		MAC1 = A1( ( R << 4 ) * IR1 );
		MAC2 = A2( ( G << 4 ) * IR2 );
		MAC3 = A3( ( B << 4 ) * IR3 );
		IR1 = Lm_B1( MAC1, lm );
		IR2 = Lm_B2( MAC2, lm );
		IR3 = Lm_B3( MAC3, lm );
		RGB0 = RGB1;
		RGB1 = RGB2;
		CD2 = CODE;
		R2 = Lm_C1( MAC1 >> 4 );
		G2 = Lm_C2( MAC2 >> 4 );
		B2 = Lm_C3( MAC3 >> 4 );
		return 1;

	case 0x1c:
		GTELOG( pc, "%08x CC", gteop );

		MAC1 = A1( int44( ( (int64_t) RBK ) << 12 ) + ( LR1 * IR1 ) + ( LR2 * IR2 ) + ( LR3 * IR3 ) );
		MAC2 = A2( int44( ( (int64_t) GBK ) << 12 ) + ( LG1 * IR1 ) + ( LG2 * IR2 ) + ( LG3 * IR3 ) );
		MAC3 = A3( int44( ( (int64_t) BBK ) << 12 ) + ( LB1 * IR1 ) + ( LB2 * IR2 ) + ( LB3 * IR3 ) );
		IR1 = Lm_B1( MAC1, lm );
		IR2 = Lm_B2( MAC2, lm );
		IR3 = Lm_B3( MAC3, lm );
		MAC1 = A1( ( R << 4 ) * IR1 );
		MAC2 = A2( ( G << 4 ) * IR2 );
		MAC3 = A3( ( B << 4 ) * IR3 );
		IR1 = Lm_B1( MAC1, lm );
		IR2 = Lm_B2( MAC2, lm );
		IR3 = Lm_B3( MAC3, lm );
		RGB0 = RGB1;
		RGB1 = RGB2;
		CD2 = CODE;
		R2 = Lm_C1( MAC1 >> 4 );
		G2 = Lm_C2( MAC2 >> 4 );
		B2 = Lm_C3( MAC3 >> 4 );
		return 1;

	case 0x1e:
		GTELOG( pc, "%08x NCS", gteop );

		MAC1 = A1( (int64_t) ( L11 * VX0 ) + ( L12 * VY0 ) + ( L13 * VZ0 ) );
		MAC2 = A2( (int64_t) ( L21 * VX0 ) + ( L22 * VY0 ) + ( L23 * VZ0 ) );
		MAC3 = A3( (int64_t) ( L31 * VX0 ) + ( L32 * VY0 ) + ( L33 * VZ0 ) );
		IR1 = Lm_B1( MAC1, lm );
		IR2 = Lm_B2( MAC2, lm );
		IR3 = Lm_B3( MAC3, lm );
		MAC1 = A1( int44( (int64_t) RBK << 12 ) + ( LR1 * IR1 ) + ( LR2 * IR2 ) + ( LR3 * IR3 ) );
		MAC2 = A2( int44( (int64_t) GBK << 12 ) + ( LG1 * IR1 ) + ( LG2 * IR2 ) + ( LG3 * IR3 ) );
		MAC3 = A3( int44( (int64_t) BBK << 12 ) + ( LB1 * IR1 ) + ( LB2 * IR2 ) + ( LB3 * IR3 ) );
		IR1 = Lm_B1( MAC1, lm );
		IR2 = Lm_B2( MAC2, lm );
		IR3 = Lm_B3( MAC3, lm );
		RGB0 = RGB1;
		RGB1 = RGB2;
		CD2 = CODE;
		R2 = Lm_C1( MAC1 >> 4 );
		G2 = Lm_C2( MAC2 >> 4 );
		B2 = Lm_C3( MAC3 >> 4 );
		return 1;

	case 0x20:
		GTELOG( pc, "%08x NCT", gteop );

		for( v = 0; v < 3; v++ )
		{
			MAC1 = A1( (int64_t) ( L11 * VX( v ) ) + ( L12 * VY( v ) ) + ( L13 * VZ( v ) ) );
			MAC2 = A2( (int64_t) ( L21 * VX( v ) ) + ( L22 * VY( v ) ) + ( L23 * VZ( v ) ) );
			MAC3 = A3( (int64_t) ( L31 * VX( v ) ) + ( L32 * VY( v ) ) + ( L33 * VZ( v ) ) );
			IR1 = Lm_B1( MAC1, lm );
			IR2 = Lm_B2( MAC2, lm );
			IR3 = Lm_B3( MAC3, lm );
			MAC1 = A1( int44( (int64_t) RBK << 12 ) + ( LR1 * IR1 ) + ( LR2 * IR2 ) + ( LR3 * IR3 ) );
			MAC2 = A2( int44( (int64_t) GBK << 12 ) + ( LG1 * IR1 ) + ( LG2 * IR2 ) + ( LG3 * IR3 ) );
			MAC3 = A3( int44( (int64_t) BBK << 12 ) + ( LB1 * IR1 ) + ( LB2 * IR2 ) + ( LB3 * IR3 ) );
			IR1 = Lm_B1( MAC1, lm );
			IR2 = Lm_B2( MAC2, lm );
			IR3 = Lm_B3( MAC3, lm );
			RGB0 = RGB1;
			RGB1 = RGB2;
			CD2 = CODE;
			R2 = Lm_C1( MAC1 >> 4 );
			G2 = Lm_C2( MAC2 >> 4 );
			B2 = Lm_C3( MAC3 >> 4 );
		}
		return 1;

	case 0x28:
		GTELOG( pc, "%08x SQR", gteop );

		MAC1 = A1( IR1 * IR1 );
		MAC2 = A2( IR2 * IR2 );
		MAC3 = A3( IR3 * IR3 );
		IR1 = Lm_B1( MAC1, lm );
		IR2 = Lm_B2( MAC2, lm );
		IR3 = Lm_B3( MAC3, lm );
		return 1;

	case 0x1a: // end of NCDT
	case 0x29:
		GTELOG( pc, "%08x DPCL", gteop );

		MAC1 = A1( ( ( R << 4 ) * IR1 ) + ( IR0 * Lm_B1( A1( ( (int64_t) RFC << 12 ) - ( ( R << 4 ) * IR1 ) ), 0 ) ) );
		MAC2 = A2( ( ( G << 4 ) * IR2 ) + ( IR0 * Lm_B2( A2( ( (int64_t) GFC << 12 ) - ( ( G << 4 ) * IR2 ) ), 0 ) ) );
		MAC3 = A3( ( ( B << 4 ) * IR3 ) + ( IR0 * Lm_B3( A3( ( (int64_t) BFC << 12 ) - ( ( B << 4 ) * IR3 ) ), 0 ) ) );
		IR1 = Lm_B1( MAC1, lm );
		IR2 = Lm_B2( MAC2, lm );
		IR3 = Lm_B3( MAC3, lm );
		RGB0 = RGB1;
		RGB1 = RGB2;
		CD2 = CODE;
		R2 = Lm_C1( MAC1 >> 4 );
		G2 = Lm_C2( MAC2 >> 4 );
		B2 = Lm_C3( MAC3 >> 4 );
		return 1;

	case 0x2a:
		GTELOG( pc, "%08x DPCT", gteop );

		for( v = 0; v < 3; v++ )
		{
			MAC1 = A1( ( R0 << 16 ) + ( IR0 * Lm_B1( A1( ( (int64_t) RFC << 12 ) - ( R0 << 16 ) ), 0 ) ) );
			MAC2 = A2( ( G0 << 16 ) + ( IR0 * Lm_B2( A2( ( (int64_t) GFC << 12 ) - ( G0 << 16 ) ), 0 ) ) );
			MAC3 = A3( ( B0 << 16 ) + ( IR0 * Lm_B3( A3( ( (int64_t) BFC << 12 ) - ( B0 << 16 ) ), 0 ) ) );
			IR1 = Lm_B1( MAC1, lm );
			IR2 = Lm_B2( MAC2, lm );
			IR3 = Lm_B3( MAC3, lm );
			RGB0 = RGB1;
			RGB1 = RGB2;
			CD2 = CODE;
			R2 = Lm_C1( MAC1 >> 4 );
			G2 = Lm_C2( MAC2 >> 4 );
			B2 = Lm_C3( MAC3 >> 4 );
		}
		return 1;

	case 0x2d:
		GTELOG( pc, "%08x AVSZ3", gteop );

		MAC0 = F( (int64_t) ( ZSF3 * SZ1 ) + ( ZSF3 * SZ2 ) + ( ZSF3 * SZ3 ) );
		OTZ = Lm_D( m_mac0, 1 );
		return 1;

	case 0x2e:
		GTELOG( pc, "%08x AVSZ4", gteop );

		MAC0 = F( (int64_t) ( ZSF4 * SZ0 ) + ( ZSF4 * SZ1 ) + ( ZSF4 * SZ2 ) + ( ZSF4 * SZ3 ) );
		OTZ = Lm_D( m_mac0, 1 );
		return 1;

	case 0x30:
		GTELOG( pc, "%08x RTPT", gteop );

		for( v = 0; v < 3; v++ )
		{
			MAC1 = A1( int44( (int64_t) TRX << 12 ) + ( R11 * VX( v ) ) + ( R12 * VY( v ) ) + ( R13 * VZ( v ) ) );
			MAC2 = A2( int44( (int64_t) TRY << 12 ) + ( R21 * VX( v ) ) + ( R22 * VY( v ) ) + ( R23 * VZ( v ) ) );
			MAC3 = A3( int44( (int64_t) TRZ << 12 ) + ( R31 * VX( v ) ) + ( R32 * VY( v ) ) + ( R33 * VZ( v ) ) );
			IR1 = Lm_B1( MAC1, lm );
			IR2 = Lm_B2( MAC2, lm );
			IR3 = Lm_B3_sf( m_mac3, m_sf, lm );
			SZ0 = SZ1;
			SZ1 = SZ2;
			SZ2 = SZ3;
			SZ3 = Lm_D( m_mac3, 1 );
			h_over_sz3 = Lm_E( gte_divide( H, SZ3 ) );
			SXY0 = SXY1;
			SXY1 = SXY2;
			SX2 = Lm_G1( F( (int64_t) OFX + ( (int64_t) IR1 * h_over_sz3 ) ) >> 16 );
			SY2 = Lm_G2( F( (int64_t) OFY + ( (int64_t) IR2 * h_over_sz3 ) ) >> 16 );
			if( m_pgxp_enabled )
			{
				pgxp_write_shadow( IR1, IR2, SZ3 );
			}
		}

		MAC0 = F( (int64_t) DQB + ( (int64_t) DQA * h_over_sz3 ) );
		IR0 = Lm_H( m_mac0, 1 );
		return 1;

	case 0x3d:
		GTELOG( pc, "%08x GPF", gteop );

		MAC1 = A1( IR0 * IR1 );
		MAC2 = A2( IR0 * IR2 );
		MAC3 = A3( IR0 * IR3 );
		IR1 = Lm_B1( MAC1, lm );
		IR2 = Lm_B2( MAC2, lm );
		IR3 = Lm_B3( MAC3, lm );
		RGB0 = RGB1;
		RGB1 = RGB2;
		CD2 = CODE;
		R2 = Lm_C1( MAC1 >> 4 );
		G2 = Lm_C2( MAC2 >> 4 );
		B2 = Lm_C3( MAC3 >> 4 );
		return 1;

	case 0x3e:
		GTELOG( pc, "%08x GPL", gteop );

		MAC1 = A1( gte_shift( MAC1, -m_sf ) + ( IR0 * IR1 ) );
		MAC2 = A2( gte_shift( MAC2, -m_sf ) + ( IR0 * IR2 ) );
		MAC3 = A3( gte_shift( MAC3, -m_sf ) + ( IR0 * IR3 ) );
		IR1 = Lm_B1( MAC1, lm );
		IR2 = Lm_B2( MAC2, lm );
		IR3 = Lm_B3( MAC3, lm );
		RGB0 = RGB1;
		RGB1 = RGB2;
		CD2 = CODE;
		R2 = Lm_C1( MAC1 >> 4 );
		G2 = Lm_C2( MAC2 >> 4 );
		B2 = Lm_C3( MAC3 >> 4 );
		return 1;

	case 0x3f:
		GTELOG( pc, "%08x NCCT", gteop );

		for( v = 0; v < 3; v++ )
		{
			MAC1 = A1( (int64_t) ( L11 * VX( v ) ) + ( L12 * VY( v ) ) + ( L13 * VZ( v ) ) );
			MAC2 = A2( (int64_t) ( L21 * VX( v ) ) + ( L22 * VY( v ) ) + ( L23 * VZ( v ) ) );
			MAC3 = A3( (int64_t) ( L31 * VX( v ) ) + ( L32 * VY( v ) ) + ( L33 * VZ( v ) ) );
			IR1 = Lm_B1( MAC1, lm );
			IR2 = Lm_B2( MAC2, lm );
			IR3 = Lm_B3( MAC3, lm );
			MAC1 = A1( int44( (int64_t) RBK << 12 ) + ( LR1 * IR1 ) + ( LR2 * IR2 ) + ( LR3 * IR3 ) );
			MAC2 = A2( int44( (int64_t) GBK << 12 ) + ( LG1 * IR1 ) + ( LG2 * IR2 ) + ( LG3 * IR3 ) );
			MAC3 = A3( int44( (int64_t) BBK << 12 ) + ( LB1 * IR1 ) + ( LB2 * IR2 ) + ( LB3 * IR3 ) );
			IR1 = Lm_B1( MAC1, lm );
			IR2 = Lm_B2( MAC2, lm );
			IR3 = Lm_B3( MAC3, lm );
			MAC1 = A1( ( R << 4 ) * IR1 );
			MAC2 = A2( ( G << 4 ) * IR2 );
			MAC3 = A3( ( B << 4 ) * IR3 );
			IR1 = Lm_B1( MAC1, lm );
			IR2 = Lm_B2( MAC2, lm );
			IR3 = Lm_B3( MAC3, lm );
			RGB0 = RGB1;
			RGB1 = RGB2;
			CD2 = CODE;
			R2 = Lm_C1( MAC1 >> 4 );
			G2 = Lm_C2( MAC2 >> 4 );
			B2 = Lm_C3( MAC3 >> 4 );
		}
		return 1;
	}

	//popmessage( "unknown GTE op %08x", gteop );
	//logerror( "%08x: unknown GTE op %08x\n", pc, gteop );

	return 0;
}
