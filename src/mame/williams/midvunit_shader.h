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
    offset, dither flag, target scale (samples per native pixel in each
    direction), bilinear flag, then the smallest u, smallest v, largest u
    and largest v of the quad's vertices as float bits (the range the
    texture coordinates are held to).

***************************************************************************/
#ifndef MAME_WILLIAMS_MIDVUNIT_SHADER_H
#define MAME_WILLIAMS_MIDVUNIT_SHADER_H

#pragma once

constexpr int MIDVUNIT_SHADER_STATE_WORDS = 10;
constexpr int MIDVUNIT_SHADER_VARYINGS = 2;

constexpr char MIDVUNIT_FRAGMENT_SOURCE[] =
	"#define S_MODE 0\n"
	"#define S_PIXDATA 1\n"
	"#define S_TEXBASE 2\n"
	"#define S_DITHER 3\n"
	"#define S_SCALE 4\n"
	"#define S_FILTER 5\n"
	"#define S_UMIN 6\n"
	"#define S_VMIN 7\n"
	"#define S_UMAX 8\n"
	"#define S_VMAX 9\n"
	"#define MODE_TEX 0\n"
	"#define MODE_TEXTRANS 1\n"
	"#define MODE_TEXTRANSMASK 2\n"
	"#define MODE_FLAT 3\n"
	"#define TEX_RAM_MASK 0x3fffff\n"
	"ivec3 palette_rgb(int index)\n"
	"{\n"
	"	return ivec3(texelFetch(palette, ivec2(index & 255, index >> 8), 0).zyx * 255.0 + 0.5);\n"
	"}\n"
	"int texel_at(int base, int tu, int tv)\n"
	"{\n"
	"	int addr = (base + ((tv & 255) << 8) + tu) & TEX_RAM_MASK;\n"
	"	return int(texelFetch(tex_ram, ivec2(addr & 2047, addr >> 11), 0).r);\n"
	"}\n"
	"bool shade(out vec4 color)\n"
	"{\n"
	"	ivec2 p = ivec2(frag_pos()) / state(S_SCALE);\n"
	"	if (state(S_DITHER) != 0 && ((p.x ^ p.y) & 1) != 0)\n"
	"		return false;\n"
	"	int mode = state(S_MODE);\n"
	"	int index = state(S_PIXDATA);\n"
	"	if (mode == MODE_FLAT)\n"
	"	{\n"
	"		color = vec4(vec3(palette_rgb(index & 0x7fff)) / 255.0, 1.0);\n"
	"		return true;\n"
	"	}\n"
	"	vec2 uv = clamp(vec2(v0, v1), vec2(intBitsToFloat(state(S_UMIN)), intBitsToFloat(state(S_VMIN))),\n"
	"			vec2(intBitsToFloat(state(S_UMAX)), intBitsToFloat(state(S_VMAX))));\n"
	"	if (state(S_FILTER) != 0)\n"
	"	{\n"
	"		vec2 s = uv - 0.5;\n"
	"		ivec2 t = ivec2(floor(s));\n"
	"		vec2 f = s - floor(s);\n"
	"		vec3 sum = vec3(0.0);\n"
	"		float share = 0.0;\n"
	"		for (int j = 0; j < 2; j++)\n"
	"		{\n"
	"			for (int i = 0; i < 2; i++)\n"
	"			{\n"
	"				int texel = texel_at(state(S_TEXBASE), t.x + i, t.y + j);\n"
	"				if (mode != MODE_TEX && texel == 0)\n"
	"					continue;\n"
	"				float weight = ((i == 0) ? 1.0 - f.x : f.x) * ((j == 0) ? 1.0 - f.y : f.y);\n"
	"				int tap = index;\n"
	"				if (mode == MODE_TEX || mode == MODE_TEXTRANS)\n"
	"					tap += texel;\n"
	"				sum += weight * texelFetch(palette, ivec2((tap & 255), (tap & 0x7fff) >> 8), 0).zyx;\n"
	"				share += weight;\n"
	"			}\n"
	"		}\n"
	"		if (share < 0.5)\n"
	"			return false;\n"
	"		color = vec4(sum / share, 1.0);\n"
	"		return true;\n"
	"	}\n"
	"	int texel = texel_at(state(S_TEXBASE), int(floor(uv.x)), int(floor(uv.y)));\n"
	"	if (mode != MODE_TEX && texel == 0)\n"
	"		return false;\n"
	"	if (mode == MODE_TEX || mode == MODE_TEXTRANS)\n"
	"		index += texel;\n"
	"	color = vec4(vec3(palette_rgb(index & 0x7fff)) / 255.0, 1.0);\n"
	"	return true;\n"
	"}\n";

#endif // MAME_WILLIAMS_MIDVUNIT_SHADER_H
