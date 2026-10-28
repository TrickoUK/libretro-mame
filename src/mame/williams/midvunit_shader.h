// license:BSD-3-Clause
// copyright-holders:TrickoUK
/***************************************************************************

    midvunit_shader.h

    Pixel pipeline of the Midway V-Unit host-GPU path as a gpu_shader_module
    (see osd/interface/gpuengine.h). A quad is the textured or flat fill the
    chip makes: the texel is an 8-bit index into texture RAM, the colour is
    the index plus the quad's palette base, looked up in the palette at draw
    time. Transparent texels are skipped in the two transparent modes, and the
    checkerboard dither keeps every other pixel of the page.

    Source: my-docs/tools/midvunit.glsl, converted by tools/glsl2header.py.
    Bindings: "tex_ram" (R8UI, 2048 wide, the texture RAM) and "palette"
    (RGBA8, 256 x 128, the 0x8000 pens). Varyings are screen-linear: v0 = u,
    v1 = v, in texel units.

    State record, MIDVUNIT_SHADER_STATE_WORDS words: mode (0 textured, 1
    textured with transparent texel 0, 2 textured with the quad colour
    where the texel is set, 3 flat), palette index base, texture base byte
    offset, dither flag.

***************************************************************************/
#ifndef MAME_WILLIAMS_MIDVUNIT_SHADER_H
#define MAME_WILLIAMS_MIDVUNIT_SHADER_H

#pragma once

constexpr int MIDVUNIT_SHADER_STATE_WORDS = 4;
constexpr int MIDVUNIT_SHADER_VARYINGS = 2;

constexpr char MIDVUNIT_FRAGMENT_SOURCE[] =
	"#define S_MODE 0\n"
	"#define S_PIXDATA 1\n"
	"#define S_TEXBASE 2\n"
	"#define S_DITHER 3\n"
	"#define MODE_TEX 0\n"
	"#define MODE_TEXTRANS 1\n"
	"#define MODE_TEXTRANSMASK 2\n"
	"#define MODE_FLAT 3\n"
	"#define TEX_RAM_MASK 0x3fffff\n"
	"ivec3 palette_rgb(int index)\n"
	"{\n"
	"	return ivec3(texelFetch(palette, ivec2(index & 255, index >> 8), 0).zyx * 255.0 + 0.5);\n"
	"}\n"
	"bool shade(out vec4 color)\n"
	"{\n"
	"	ivec2 p = ivec2(frag_pos());\n"
	"	if (state(S_DITHER) != 0 && ((p.x ^ p.y) & 1) != 0)\n"
	"		return false;\n"
	"	int mode = state(S_MODE);\n"
	"	int index = state(S_PIXDATA);\n"
	"	if (mode != MODE_FLAT)\n"
	"	{\n"
	"		int addr = (state(S_TEXBASE) + ((int(floor(v1)) & 255) << 8) + int(floor(v0))) & TEX_RAM_MASK;\n"
	"		int texel = int(texelFetch(tex_ram, ivec2(addr & 2047, addr >> 11), 0).r);\n"
	"		if (mode != MODE_TEX && texel == 0)\n"
	"			return false;\n"
	"		if (mode == MODE_TEX || mode == MODE_TEXTRANS)\n"
	"			index += texel;\n"
	"	}\n"
	"	color = vec4(vec3(palette_rgb(index & 0x7fff)) / 255.0, 1.0);\n"
	"	return true;\n"
	"}\n";

#endif // MAME_WILLIAMS_MIDVUNIT_SHADER_H
