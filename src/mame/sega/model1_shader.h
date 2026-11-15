// license:BSD-3-Clause
// copyright-holders:TrickoUK
/***************************************************************************

    model1_shader.h

    Pixel pipeline of the Sega Model 1 host-GPU path as a gpu_shader_module
    (see osd/interface/gpuengine.h). The polygons are flat shaded, so a
    primitive's colour comes straight from its state record; the moire
    stipple drops every other native pixel in a checkerboard, as the
    scanline filler in model1_v.cpp does.

    Source: my-docs/tools/model1.glsl, converted by tools/glsl2header.py.
    No bindings, no varyings.

    State record, MODEL1_SHADER_STATE_WORDS words: colour (0x00rrggbb),
    flags (1 moire) and the target pixels per native pixel.

***************************************************************************/
#ifndef MAME_SEGA_MODEL1_SHADER_H
#define MAME_SEGA_MODEL1_SHADER_H

#pragma once

constexpr int MODEL1_SHADER_STATE_WORDS = 3;
constexpr int MODEL1_SHADER_VARYINGS = 0;

constexpr char MODEL1_FRAGMENT_SOURCE[] =
	"bool shade(out vec4 color)\n"
	"{\n"
	"	int flags = state(1);\n"
	"	if ((flags & 1) != 0)\n"
	"	{\n"
	"		ivec2 p = ivec2(frag_pos()) / state(2);\n"
	"		if (((p.x ^ p.y) & 1) != 0)\n"
	"			return false;\n"
	"	}\n"
	"	int c = state(0);\n"
	"	color = vec4(float((c >> 16) & 255), float((c >> 8) & 255), float(c & 255), 255.0) / 255.0;\n"
	"	return true;\n"
	"}\n";

#endif // MAME_SEGA_MODEL1_SHADER_H
