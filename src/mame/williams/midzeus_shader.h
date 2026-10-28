// license:BSD-3-Clause
// copyright-holders:TrickoUK
/***************************************************************************

    midzeus_shader.h

    Pixel pipeline of the Midway Zeus 1 host-GPU path as a gpu_shader_module
    (see osd/interface/gpuengine.h). A polygon is a textured quad, a
    solid-colour quad or the solid fill that clears the screen. A texel is an
    index into wave RAM 0 (4 or 8 bits, in the two block layouts of the chip),
    the bilinear filter runs after the palette lookup, and the colour is kept
    to 5 bits per channel as the frame buffer stores it. The depth is the
    chip's integer depth, written to frag_depth.

    Source: my-docs/tools/midzeus.glsl, converted by tools/glsl2header.py.
    Binding: "tex_ram" (R8UI, 4096 x 2048, wave RAM 0 one byte per texel).
    MIDZEUS_STAMP_SOURCE is the second module, the frame-buffer write stamp
    (my-docs/tools/midzeus_stamp.glsl): binding "tex_fb" (R16UI, 1024 x 1024,
    the frame buffer words), one colour or depth plane per draw variant.
    Varyings are screen-linear: v0 = u and v1 = v in texel units, v2 = z in
    the chip's 16.16 units.

    State record, MIDZEUS_SHADER_STATE_WORDS words: kind (0 textured, 1
    solid colour, 2 fill), colour, texture base byte offset, texture width,
    block layout, v offset, palette base byte offset, colour key flag, blend
    (0 plain, 1 intensity 255/256, 2 srca/256, 3 2 * srca/256), srca, depth
    offset, depth test flag, supersample scale (samples per native pixel along
    each side; the write stamp reads it).

***************************************************************************/
#ifndef MAME_WILLIAMS_MIDZEUS_SHADER_H
#define MAME_WILLIAMS_MIDZEUS_SHADER_H

#pragma once

constexpr int MIDZEUS_SHADER_STATE_WORDS = 13;
constexpr int MIDZEUS_SHADER_VARYINGS = 3;

constexpr char MIDZEUS_FRAGMENT_SOURCE[] =
	"#define S_KIND 0\n"
	"#define S_COLOR 1\n"
	"#define S_TEXBASE 2\n"
	"#define S_TEXWIDTH 3\n"
	"#define S_LAYOUT 4\n"
	"#define S_VOFFSET 5\n"
	"#define S_PALBASE 6\n"
	"#define S_KEY 7\n"
	"#define S_BLEND 8\n"
	"#define S_SRCA 9\n"
	"#define S_ZOFFSET 10\n"
	"#define S_DEPTH_TEST 11\n"
	"#define S_SCALE 12\n"
	"#define KIND_TEX 0\n"
	"#define KIND_SOLID 1\n"
	"#define KIND_FILL 2\n"
	"#define LAYOUT_4BIT 0\n"
	"#define LAYOUT_8BIT 1\n"
	"#define LAYOUT_ALT_4BIT 2\n"
	"#define LAYOUT_ALT_8BIT 3\n"
	"#define BLEND_PLAIN 0\n"
	"#define BLEND_OPAQUE_FACTOR 1\n"
	"#define BLEND_SRCA_FACTOR 2\n"
	"#define BLEND_MUL_FACTOR 3\n"
	"#define WAVERAM_MASK 0x7fffff\n"
	"int ram_byte(int addr)\n"
	"{\n"
	"	addr &= WAVERAM_MASK;\n"
	"	return int(texelFetch(tex_ram, ivec2(addr & 4095, addr >> 12), 0).r);\n"
	"}\n"
	"int texel_at(int x, int y)\n"
	"{\n"
	"	int base = state(S_TEXBASE);\n"
	"	int width = state(S_TEXWIDTH);\n"
	"	int block = state(S_LAYOUT);\n"
	"	if (block == LAYOUT_8BIT)\n"
	"		return ram_byte(base + (y / 2) * (width * 2) + ((x / 4) << 3) + ((y & 1) << 2) + (x & 3));\n"
	"	if (block == LAYOUT_ALT_8BIT)\n"
	"		return ram_byte(base + (y / 4) * (width * 4) + ((x / 2) << 3) + ((y & 3) << 1) + (x & 1));\n"
	"	if (block == LAYOUT_4BIT)\n"
	"		return (ram_byte(base + (y / 2) * (width * 2) + ((x / 8) << 3) + ((y & 1) << 2) + ((x / 2) & 3)) >> (4 * (x & 1))) & 15;\n"
	"	return (ram_byte(base + (y / 4) * (width * 4) + ((x / 4) << 3) + ((y & 3) << 1) + ((x / 2) & 1)) >> (4 * (x & 1))) & 15;\n"
	"}\n"
	"int palette_word(int index)\n"
	"{\n"
	"	int addr = state(S_PALBASE) + 2 * index;\n"
	"	return ram_byte(addr) | (ram_byte(addr + 1) << 8);\n"
	"}\n"
	"ivec3 channels5(int word)\n"
	"{\n"
	"	return ivec3((word >> 10) & 31, (word >> 5) & 31, word & 31);\n"
	"}\n"
	"ivec3 expand5(ivec3 c)\n"
	"{\n"
	"	return (c << 3) | (c >> 2);\n"
	"}\n"
	"bool shade(out vec4 color)\n"
	"{\n"
	"	int depth = min(int(floor(v2 / 65536.0)) + state(S_ZOFFSET), 0x7fff);\n"
	"	if (state(S_DEPTH_TEST) != 0 && depth < 0)\n"
	"		return false;\n"
	"	frag_depth = clamp(float(depth) / 65536.0, 0.0, 1.0);\n"
	"	if (state(S_KIND) == KIND_FILL)\n"
	"	{\n"
	"		color = vec4(vec3(expand5(channels5(state(S_COLOR)))) / 255.0, 1.0);\n"
	"		return true;\n"
	"	}\n"
	"	ivec3 src;\n"
	"	if (state(S_KIND) == KIND_SOLID)\n"
	"	{\n"
	"		src = expand5(channels5(state(S_COLOR)));\n"
	"	}\n"
	"	else\n"
	"	{\n"
	"		float fu = v0 - floor(v0);\n"
	"		float fv = v1 - floor(v1);\n"
	"		int u = int(floor(v0));\n"
	"		int v = state(S_VOFFSET) + int(floor(v1));\n"
	"		int t[4];\n"
	"		t[0] = texel_at(u, v);\n"
	"		t[1] = texel_at(u + 1, v);\n"
	"		t[2] = texel_at(u, v + 1);\n"
	"		t[3] = texel_at(u + 1, v + 1);\n"
	"		if (state(S_KEY) != 0)\n"
	"		{\n"
	"			int nearest = t[((fv > 0.5) ? 2 : 0) + ((fu > 0.5) ? 1 : 0)];\n"
	"			if (nearest == 0)\n"
	"				return false;\n"
	"			for (int i = 0; i < 4; i++)\n"
	"				if (t[i] == 0)\n"
	"					t[i] = nearest;\n"
	"		}\n"
	"		vec3 sum = vec3(0.0);\n"
	"		sum += (1.0 - fu) * (1.0 - fv) * vec3(expand5(channels5(palette_word(t[0]))));\n"
	"		sum += fu * (1.0 - fv) * vec3(expand5(channels5(palette_word(t[1]))));\n"
	"		sum += (1.0 - fu) * fv * vec3(expand5(channels5(palette_word(t[2]))));\n"
	"		sum += fu * fv * vec3(expand5(channels5(palette_word(t[3]))));\n"
	"		src = ivec3(floor(sum));\n"
	"	}\n"
	"	int blend = state(S_BLEND);\n"
	"	if (blend == BLEND_OPAQUE_FACTOR)\n"
	"		src = (src * 255) >> 8;\n"
	"	else if (blend == BLEND_SRCA_FACTOR)\n"
	"		src = (src * state(S_SRCA)) >> 8;\n"
	"	else if (blend == BLEND_MUL_FACTOR)\n"
	"		src = (src * (state(S_SRCA) << 1)) >> 8;\n"
	"	ivec3 c5 = min(src, ivec3(255)) >> 3;\n"
	"	color = vec4(vec3(expand5(c5)) / 255.0, 1.0);\n"
	"	return true;\n"
	"}\n";

constexpr char MIDZEUS_STAMP_SOURCE[] =
	"#define S_SCALE 12\n"
	"bool shade(out vec4 color)\n"
	"{\n"
	"	ivec2 p = ivec2(floor(frag_pos())) / int(state(S_SCALE));\n"
	"	int word = int(texelFetch(tex_fb, ivec2(4 * (p.x / 2) + (p.x & 1) + 2 * VARIANT, p.y), 0).r);\n"
	"	frag_depth = float(word) / 65536.0;\n"
	"	ivec3 c = ivec3((word >> 10) & 31, (word >> 5) & 31, word & 31);\n"
	"	color = vec4(vec3((c << 3) | (c >> 2)) / 255.0, 1.0);\n"
	"	return true;\n"
	"}\n";

#endif // MAME_WILLIAMS_MIDZEUS_SHADER_H
