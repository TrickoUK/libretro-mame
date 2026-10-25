// license:BSD-3-Clause
// copyright-holders:TrickoUK
/***************************************************************************

    namcos23_shader.h

    Pixel pipeline of the Namco System 23 and Super System 23 host-GPU path
    as a gpu_shader_module (see osd/interface/gpuengine.h). It evaluates the
    same steps as render_scanline() in namcos23.cpp with 8-bit integer
    arithmetic: the two-level texture tile lookup with flips and swap, the
    C412 stencil test, the pen select, shading, the polygon and global fades
    and the alpha and half-and-half blends (as a blend weight).

    Source: my-docs/tools/namcos23.glsl, converted by tools/glsl2header.py.
    Bindings: "tile_data" (R32UI, 4096 wide, the texture ROM), "tile_map"
    (R32UI, 1024 wide, tile byte offset and attributes), "palette" (RGBA8,
    256 x 128, the 0x8000 pens as stored in memory), "stencil" (R16UI, 1024
    wide, the C412 SRAM) and "picture" (RGBA8, the screen bitmap as it stands
    before the polygons, which a stamp primitive copies into the target).
    Varyings are screen-linear: v0 = 1/z, v1 = u/z, v2 = v/z, v3 = intensity/z.

    State record, NAMCOS23_SHADER_STATE_WORDS words: flags (1 stencil, 2
    shade, 4 polygon fade, 8 colour fade, 16 blend, 32 alpha, 64 alpha for
    the whole polygon, 128 stamp, 256 texture filter), pen base, pen mask
    and shift, texture bank, tile id mask, intensity base (a float), polygon
    fade colour, fade mix and colour, alpha mix and pen, the clip rectangle
    (min x, min y, max x + 1, max y + 1 in target pixels) and the stamp scale.

***************************************************************************/
#ifndef MAME_NAMCO_NAMCOS23_SHADER_H
#define MAME_NAMCO_NAMCOS23_SHADER_H

#pragma once

constexpr int NAMCOS23_SHADER_STATE_WORDS = 21;
constexpr int NAMCOS23_SHADER_VARYINGS = 4;

constexpr char NAMCOS23_FRAGMENT_SOURCE[] =
	"#define S_FLAGS 0\n"
	"#define S_PENBASE 1\n"
	"#define S_PENMASK 2\n"
	"#define S_PENSHIFT 3\n"
	"#define S_TBASE 4\n"
	"#define S_TILEMASK 5\n"
	"#define S_INTENSITY 6\n"
	"#define S_POLYR 7\n"
	"#define S_POLYG 8\n"
	"#define S_POLYB 9\n"
	"#define S_FADEMIX 10\n"
	"#define S_FADER 11\n"
	"#define S_FADEG 12\n"
	"#define S_FADEB 13\n"
	"#define S_ALPHAMIX 14\n"
	"#define S_ALPHAPEN 15\n"
	"#define S_CLIP 16\n"
	"#define S_SCALE 20\n"
	"#define F_STENCIL 1\n"
	"#define F_SHADE 2\n"
	"#define F_PFADE 4\n"
	"#define F_CFADE 8\n"
	"#define F_BLEND 16\n"
	"#define F_ALPHA 32\n"
	"#define F_ALPHAALL 64\n"
	"#define F_STAMP 128\n"
	"#define F_FILTER 256\n"
	"ivec3 palette_rgb(int index)\n"
	"{\n"
	"	return ivec3(texelFetch(palette, ivec2(index & 255, index >> 8), 0).zyx * 255.0 + 0.5);\n"
	"}\n"
	"int rom_byte(int offset)\n"
	"{\n"
	"	int word = int(texelFetch(tile_data, ivec2((offset >> 2) & 4095, offset >> 14), 0).r);\n"
	"	return (word >> ((offset & 3) << 3)) & 255;\n"
	"}\n"
	"int tile_pen(int tx, int ty)\n"
	"{\n"
	"	int tileid = ((tx >> 4) & 255) | ((ty << 4) & state(S_TILEMASK));\n"
	"	uint entry = texelFetch(tile_map, ivec2(tileid & 1023, tileid >> 10), 0).r;\n"
	"	int attr = int(entry >> 28) & 7;\n"
	"	int u = tx;\n"
	"	int v = ty;\n"
	"	if ((attr & 1) != 0)\n"
	"		v = ~v;\n"
	"	if ((attr & 2) != 0)\n"
	"		u = ~u;\n"
	"	if ((attr & 4) != 0)\n"
	"	{\n"
	"		int t = u;\n"
	"		u = v;\n"
	"		v = t;\n"
	"	}\n"
	"	return rom_byte(int(entry & 0x0fffffffu) | ((v << 4) & 0xf0) | (u & 0x0f));\n"
	"}\n"
	"bool stencil_masked(int tx, int ty)\n"
	"{\n"
	"	int offs = ((ty << 6) | (tx >> 4)) & 0x1ffff;\n"
	"	int word = int(texelFetch(stencil, ivec2(offs & 1023, offs >> 10), 0).r);\n"
	"	return ((word >> ((tx & 15) ^ 15)) & 1) == 0;\n"
	"}\n"
	"ivec3 tap_rgb(int tx, int ty)\n"
	"{\n"
	"	int pen = tile_pen(tx, ty + state(S_TBASE));\n"
	"	return palette_rgb(state(S_PENBASE) + ((pen >> state(S_PENSHIFT)) & state(S_PENMASK)));\n"
	"}\n"
	"ivec3 filtered_rgb(float u, float v)\n"
	"{\n"
	"	float fu = u - 0.5;\n"
	"	float fv = v - 0.5;\n"
	"	int x0 = int(floor(fu));\n"
	"	int y0 = int(floor(fv));\n"
	"	float fx = fu - floor(fu);\n"
	"	float fy = fv - floor(fv);\n"
	"	vec3 top = mix(vec3(tap_rgb(x0, y0)), vec3(tap_rgb(x0 + 1, y0)), fx);\n"
	"	vec3 bottom = mix(vec3(tap_rgb(x0, y0 + 1)), vec3(tap_rgb(x0 + 1, y0 + 1)), fx);\n"
	"	return ivec3(mix(top, bottom, fy) + 0.5);\n"
	"}\n"
	"bool shade(out vec4 color)\n"
	"{\n"
	"#if ANISO_MAX > 1\n"
	"	vec2 uv_pos = vec2(v1, v2) / v0;\n"
	"	vec2 uv_dx = dFdx(uv_pos);\n"
	"	vec2 uv_dy = dFdy(uv_pos);\n"
	"#endif\n"
	"	ivec2 p = ivec2(frag_pos());\n"
	"	if (p.x < state(S_CLIP) || p.y < state(S_CLIP + 1) || p.x >= state(S_CLIP + 2) || p.y >= state(S_CLIP + 3))\n"
	"		return false;\n"
	"	int flags = state(S_FLAGS);\n"
	"	if ((flags & F_STAMP) != 0)\n"
	"	{\n"
	"		color = vec4(texelFetch(picture, p / max(state(S_SCALE), 1), 0).zyx, 0.0);\n"
	"		return true;\n"
	"	}\n"
	"	float ooz = 1.0 / v0;\n"
	"	int tx = int(v1 * ooz);\n"
	"	int ty = int(v2 * ooz);\n"
	"	if ((flags & F_STENCIL) != 0 && stencil_masked(tx, ty))\n"
	"		return false;\n"
	"	int pen = tile_pen(tx, ty + state(S_TBASE));\n"
	"	ivec3 rgb = palette_rgb(state(S_PENBASE) + ((pen >> state(S_PENSHIFT)) & state(S_PENMASK)));\n"
	"	if ((flags & F_FILTER) != 0)\n"
	"		rgb = filtered_rgb(v1 * ooz, v2 * ooz);\n"
	"#if ANISO_MAX > 1\n"
	"	{\n"
	"		vec2 axis;\n"
	"		int taps = aniso_taps(uv_dx, uv_dy, ANISO_MAX, axis);\n"
	"		if (taps > 1)\n"
	"		{\n"
	"			ivec3 sum = ivec3(0);\n"
	"			int used = 0;\n"
	"			vec2 uv = vec2(v1, v2) * ooz;\n"
	"			for (int i = 0; i < taps; i++)\n"
	"			{\n"
	"				vec2 o = axis * ((float(i) + 0.5) / float(taps) - 0.5);\n"
	"				if ((flags & F_STENCIL) != 0 && stencil_masked(int(uv.x + o.x), int(uv.y + o.y)))\n"
	"					continue;\n"
	"				sum += ((flags & F_FILTER) != 0) ? filtered_rgb(uv.x + o.x, uv.y + o.y) : tap_rgb(int(uv.x + o.x), int(uv.y + o.y));\n"
	"				used++;\n"
	"			}\n"
	"			if (used > 0)\n"
	"				rgb = (sum + ivec3(used / 2)) / used;\n"
	"		}\n"
	"	}\n"
	"#endif\n"
	"	if ((flags & F_SHADE) != 0)\n"
	"	{\n"
	"		int level = clamp(int(intBitsToFloat(state(S_INTENSITY)) + v3 * ooz), 0, 255);\n"
	"		rgb = min((rgb * level) >> 6, 255);\n"
	"	}\n"
	"	if ((flags & F_PFADE) != 0)\n"
	"		rgb = (rgb * ivec3(state(S_POLYR), state(S_POLYG), state(S_POLYB))) >> 8;\n"
	"	if ((flags & F_CFADE) != 0)\n"
	"	{\n"
	"		int mix_ = state(S_FADEMIX);\n"
	"		rgb = (rgb * mix_ + ivec3(state(S_FADER), state(S_FADEG), state(S_FADEB)) * (256 - mix_)) >> 8;\n"
	"	}\n"
	"	if ((flags & F_ALPHA) != 0 && ((flags & F_ALPHAALL) != 0 || pen == state(S_ALPHAPEN)))\n"
	"		blend_factor = float(state(S_ALPHAMIX)) / 256.0;\n"
	"	else if ((flags & F_BLEND) != 0)\n"
	"		blend_factor = 0.5;\n"
	"	color = vec4(vec3(rgb) / 255.0, 128.0 / 255.0);\n"
	"	return true;\n"
	"}\n";

#endif // MAME_NAMCO_NAMCOS23_SHADER_H
