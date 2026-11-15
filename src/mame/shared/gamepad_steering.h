// license:BSD-3-Clause
// copyright-holders:libretro-mame fork
/***************************************************************************

    Opt-in steering assists for playing wheel games with a gamepad stick
    (libretro-mame fork).

    MAME's analog sensitivity setting has no effect on absolute axes, and a
    stick can go from centre to full lock in a frame or two where a real
    wheel can't. These helpers add two Machine Configuration settings, both
    defaulting to arcade behaviour:
    - Steering Response: a power curve that shrinks small deflections around
      centre while still reaching full lock at full stick.
    - Steering Smoothing: a rate limit on the steering position, given as a
      lock-to-lock time in emulated time. Deliberately not saved in save
      states: after a load it restarts from the current input.

    Same settings and bit layout as konami/viper.cpp's gticlub2/thrild2
    steering (bits 0-1 response, bits 2-4 smoothing).

***************************************************************************/

#ifndef MAME_SHARED_GAMEPAD_STEERING_H
#define MAME_SHARED_GAMEPAD_STEERING_H

#pragma once

#include <algorithm>
#include <cmath>


#define PORT_GAMEPAD_STEERING_RESPONSE \
	PORT_CONFNAME( 0x03, 0x00, "Steering Response" ) \
	PORT_CONFSETTING(    0x00, "Linear (arcade)" ) \
	PORT_CONFSETTING(    0x01, "Mild curve" ) \
	PORT_CONFSETTING(    0x02, "Squared curve" ) \
	PORT_CONFSETTING(    0x03, "Cubic curve" )

#define PORT_GAMEPAD_STEERING_SMOOTHING \
	PORT_CONFNAME( 0x1c, 0x00, "Steering Smoothing" ) \
	PORT_CONFSETTING(    0x00, "Off (arcade)" ) \
	PORT_CONFSETTING(    0x04, "Light (0.25 s lock to lock)" ) \
	PORT_CONFSETTING(    0x08, "Medium (0.5 s)" ) \
	PORT_CONFSETTING(    0x0c, "Medium+ (0.625 s)" ) \
	PORT_CONFSETTING(    0x10, "Firm (0.75 s)" ) \
	PORT_CONFSETTING(    0x14, "Firm+ (0.875 s)" ) \
	PORT_CONFSETTING(    0x18, "Heavy (1 s)" )

// both settings in a "STEERING" port, for games with an absolute (centred) wheel
#define PORT_GAMEPAD_STEERING_CONFIG \
	PORT_START("STEERING") \
	PORT_GAMEPAD_STEERING_RESPONSE \
	PORT_GAMEPAD_STEERING_SMOOTHING


class gamepad_steering
{
public:
	// d is the deflection from centre, -1.0 to 1.0
	static double response(double d, u32 config)
	{
		static constexpr double exponents[] = { 1.0, 1.5, 2.0, 3.0 };
		const double exponent = exponents[config & 3];
		if (exponent == 1.0)
			return d;
		return std::copysign(std::pow(std::abs(d), exponent), d);
	}

	// value is a raw sample between lo and hi, centred on centre; returns the adjusted sample
	u32 apply(u32 value, u32 lo, u32 centre, u32 hi, u32 config, const attotime &now)
	{
		const double range = (value >= centre) ? double(hi - centre) : double(centre - lo);
		double d = (range > 0.0) ? (double(value) - centre) / range : 0.0;
		d = smooth(response(std::clamp(d, -1.0, 1.0), config), config, now);
		const double out_range = (d >= 0.0) ? double(hi - centre) : double(centre - lo);
		return std::clamp<int>(int(std::lround(centre + d * out_range)), lo, hi);
	}

private:
	// rate limit on the deflection; full lock to full lock is a change of 2.0
	double smooth(double target, u32 config, const attotime &now)
	{
		static constexpr double lock_to_lock_seconds[] = { 0.0, 0.25, 0.5, 0.625, 0.75, 0.875, 1.0, 1.0 };
		const unsigned mode = BIT(config, 2, 3);
		if (mode == 0 || m_time.is_zero() || now < m_time)
		{
			m_pos = target;
			m_time = now;
			return target;
		}

		const double max_step = 2.0 * (now - m_time).as_double() / lock_to_lock_seconds[mode];
		m_time = now;
		m_pos += std::clamp(target - m_pos, -max_step, max_step);
		return m_pos;
	}

	double m_pos = 0.0;
	attotime m_time = attotime::zero;
};

#endif // MAME_SHARED_GAMEPAD_STEERING_H
