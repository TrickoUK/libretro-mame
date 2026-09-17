// license:BSD-3-Clause
// copyright-holders:smf
/*
 * PlayStation Geometry Transformation Engine emulator
 *
 * Copyright 2003-2013 smf
 *
 */

#ifndef MAME_CPU_PSX_GTE_H
#define MAME_CPU_PSX_GTE_H

#pragma once

#include <cstdint>


#define GTE_SF( op ) ( ( op >> 19 ) & 1 )
#define GTE_MX( op ) ( ( op >> 17 ) & 3 )
#define GTE_V( op ) ( ( op >> 15 ) & 3 )
#define GTE_CV( op ) ( ( op >> 13 ) & 3 )
#define GTE_LM( op ) ( ( op >> 10 ) & 1 )
#define GTE_FUNCT( op ) ( op & 63 )

class gte
{
public:
	gte() : m_sf(0), m_mac0(0), m_mac1(0), m_mac2(0), m_mac3(0)
	{
	}

	PAIR m_cp2cr[ 32 ];
	PAIR m_cp2dr[ 32 ];

	uint32_t getcp2dr( uint32_t pc, int reg );
	void setcp2dr( uint32_t pc, int reg, uint32_t value );
	uint32_t getcp2cr( uint32_t pc, int reg );
	void setcp2cr( uint32_t pc, int reg, uint32_t value );
	int docop2( uint32_t pc, int gteop );

	// PGXP-style geometry/texture correction (see CLAUDE.md Phase 4c): when
	// enabled, RTPS/RTPT additionally redo their transform+perspective-
	// divide in double precision (skipping the fixed-point path's several
	// intermediate saturation/truncation steps, and reproducing the real
	// hardware clamps those steps have - see pgxp_write_shadow()'s
	// definition) and mirror the result into a 3-entry shadow FIFO
	// alongside the real SXY0/SXY1/SXY2 FIFO (m_cp2dr[12..14]) it's
	// rotated in lockstep with. This is deliberately NOT a value-keyed
	// cache (Phase 4b's approach, replaced here after live testing and a
	// reference-implementation comparison - see CLAUDE.md - showed a
	// value-keyed cache is inherently collision/staleness-prone even
	// after several rounds of fixes): a caller reads a specific FIFO slot
	// by GTE register number, exactly mirroring how the real SXY FIFO is
	// read (MFC2), so there is nothing to collide or go stale within this
	// layer. Purely additive: never changes any existing GTE register/
	// FLAG/timing behavior, and costs nothing when disabled.
	void set_pgxp_enabled( bool enabled ) { m_pgxp_enabled = enabled; }

	struct pgxp_shadow
	{
		float x = 0.0f;
		float y = 0.0f;
		float w = 1.0f;
		bool valid = false;
	};

	// reg: a cp2dr register number - 12/13/14 for SXY0/SXY1/SXY2, 15 for
	// the SXYP auto-increment alias (matches getcp2dr()'s own reg 15
	// case, which returns SXY2's value). Any other register has no
	// shadow, always returns an invalid entry.
	pgxp_shadow pgxp_shadow_for_reg( int reg ) const;

protected:
	class int44
	{
	public:
		int44( int64_t value ) :
			m_value( value ),
			m_positive_overflow( value > 0x7ffffffffff ),
			m_negative_overflow( value < -0x80000000000 )
		{
		}

		int44( int64_t value, bool positive_overflow, bool negative_overflow ) :
			m_value( value ),
			m_positive_overflow( positive_overflow ),
			m_negative_overflow( negative_overflow )
		{
		}

		int44 operator+( int64_t add )
		{
			int64_t value = util::sext( m_value + add, 44 );

			return int44( value,
				m_positive_overflow || ( value < 0 && m_value >= 0 && add >= 0 ),
				m_negative_overflow || ( value >= 0 && m_value < 0 && add < 0 ) );
		}

		bool positive_overflow()
		{
			return m_positive_overflow;
		}

		bool negative_overflow()
		{
			return m_negative_overflow;
		}

		int64_t value()
		{
			return m_value;
		}

	private:
		int64_t m_value;
		bool m_positive_overflow;
		bool m_negative_overflow;
	};

	int32_t LIM( int32_t value, int32_t max, int32_t min, uint32_t flag );
	int32_t BOUNDS( int44 a, int max_flag, int min_flag );
	int32_t A1( int44 a );
	int32_t A2( int44 a );
	int32_t A3( int44 a );
	int32_t Lm_B1( int32_t a, int lm );
	int32_t Lm_B2( int32_t a, int lm );
	int32_t Lm_B3( int32_t a, int lm );
	int32_t Lm_B3_sf( int64_t value, int sf, int lm );
	int32_t Lm_C1( int32_t a );
	int32_t Lm_C2( int32_t a );
	int32_t Lm_C3( int32_t a );
	int32_t Lm_D( int64_t a, int sf );
	uint32_t Lm_E( uint32_t result );
	int64_t F( int64_t a );
	int32_t Lm_G1( int64_t a );
	int32_t Lm_G2( int64_t a );
	int32_t Lm_H( int64_t value, int sf );

	int m_sf;
	int64_t m_mac0;
	int64_t m_mac1;
	int64_t m_mac2;
	int64_t m_mac3;

	// Float redo of RTPS/RTPT's perspective-divide step, using the
	// hardware-clamped IR1/IR2 (using the raw pre-clamp MAC1/MAC2 was
	// tried first and was a real bug - for a near-degenerate vertex
	// A1()/A2() never actually clamp their return value, only flag
	// overflow, so MAC1/MAC2 could reach values that multiplied into a
	// screen position literally millions of pixels off). Also reproduces
	// two more hardware clamps this recomputation would otherwise skip:
	// h_over_sz3 (gte_divide()+Lm_E()) is capped to 0x1ffff/65536 - not a
	// rare case, this triggers for any vertex closer than half the "H"
	// reference distance, i.e. ordinary foreground geometry - and the
	// final x/y is capped to [-1024,1023] (Lm_G1/Lm_G2), hardware's own
	// safety net for a near-zero SZ3 blowing up the divide regardless of
	// how IR1/IR2 are bounded. Rotates the result into m_pgxp_shadow[]
	// exactly where docop2() rotates SXY0/SXY1/SXY2 - called once per
	// vertex, only when m_pgxp_enabled.
	void pgxp_write_shadow( int64_t ir1, int64_t ir2, int32_t sz3 );

	bool m_pgxp_enabled = false;
	pgxp_shadow m_pgxp_shadow[ 3 ];
};

#endif // MAME_CPU_PSX_GTE_H
