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

	// PGXP-style geometry/texture correction (see CLAUDE.md Phase 4): when
	// enabled, RTPS/RTPT additionally redo their transform+perspective-
	// divide in double precision (skipping the fixed-point path's several
	// intermediate saturation/truncation steps) and cache the result here
	// - x/y for geometry correction, w (the vertex's real eye-space depth,
	// SZ3) for perspective-correct texture/color interpolation - keyed by
	// the exact fixed-point SXY word just written to m_cp2dr[12..14] - the
	// same word a game copies verbatim into a later GP0 polygon command,
	// and so the only thing available to match a later primitive's vertex
	// back up with the higher-precision value that produced it. Purely
	// additive: never changes any existing GTE register/FLAG/timing
	// behavior, and costs nothing when disabled.
	void set_pgxp_enabled( bool enabled ) { m_pgxp_enabled = enabled; }
	bool pgxp_query( uint32_t sxy_word, float &x, float &y, float &w ) const;

	// Discards every cached entry - called once per emulated frame (see
	// psxgpu_device::gpu_update_screen()). Without this, an entry that
	// survives untouched from a previous frame could coincidentally share
	// its exact fixed-point key with an unrelated vertex in a later frame
	// (the cache is small relative to how many distinct on-screen
	// positions a game can produce over multiple frames) and be used as a
	// silently wrong "hit" for that unrelated vertex - a source of
	// intermittent, frame-to-frame flicker distinct from the intra-frame
	// capacity/collision issue PGXP_CACHE_SIZE and pgxp_hash() address.
	void pgxp_clear_cache();

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

	// float redo of RTPS/RTPT's rotation+translation+perspective-divide,
	// using the already-computed full-precision MAC1/MAC2/MAC3 (not the
	// saturated IR1/IR2/IR3) as input - called once per vertex, after the
	// existing fixed-point SXY write, only when m_pgxp_enabled.
	void pgxp_cache_vertex( uint32_t sxy_word, int64_t mac1, int64_t mac2, int32_t sz3 );

	bool m_pgxp_enabled = false;

	// Large enough to comfortably hold every vertex a busy 3D scene
	// transforms in one frame (thousands is realistic) without evicting
	// entries before the matching GP0 primitive consumes them - too small
	// a cache was found to cause visible per-triangle correction dropouts
	// (a vertex either gets its cached value or silently falls back,
	// see gpu_resolve_polygon_pgxp()'s all-or-nothing rule) that flicker
	// frame to frame, and cause seams where two adjacent primitives
	// sharing a vertex land on different (corrected vs. fallback)
	// positions for what should be the exact same point.
	static constexpr int PGXP_CACHE_SIZE = 16384;
	static uint32_t pgxp_hash( uint32_t key );
	struct pgxp_entry
	{
		uint32_t key = 0;
		float x = 0.0f;
		float y = 0.0f;
		float w = 1.0f;
		bool valid = false;
	};
	pgxp_entry m_pgxp_cache[ PGXP_CACHE_SIZE ];
};

#endif // MAME_CPU_PSX_GTE_H
