// license:BSD-3-Clause
// copyright-holders:TrickoUK
/***************************************************************************

    namcos22_shader.h

    Pixel pipeline of the Namco System 22 and Super System 22 host-GPU path
    as a gpu_shader_module (see osd/interface/gpuengine.h). It evaluates the
    same steps as the scanline routines in namcos22_v.cpp with 8-bit integer
    arithmetic: the two-level texture tile lookup, the pen select, fog,
    shading, the polygon and global fades and the sprite fetch.

    Source: my-docs/tools/namcos22.glsl, converted by tools/glsl2header.py.
    Bindings: "tile_data" (R8UI, 4096 wide, the 16x16x8bpp texture tiles),
    "tile_map" (R16UI, 1024 x 1024), "tile_attr" (R8UI, 1024 x 1024),
    "sprite_data" (R8UI, 4096 wide, the 32x32x8bpp sprite tiles) and
    "palette" (RGBA8, 256 x 128, the 0x8000 pens as stored in memory) and
    "picture" (RGBA8, the screen bitmap as it stands before the polygons, which
    a stamp primitive copies into the target).
    Varyings are screen-linear: v0 = 1/z, v1 = u/z, v2 = v/z, v3 = brightness/z
    for polygons; v0 and v1 are the texel position for sprites.

    State record, NAMCOS22_SHADER_STATE_WORDS words: flags (1 sprite, 2
    texture, 4 shade, 8 Super System 22, 16 polygon fade, 32 alpha, 64 stamp, 128 texture filter), pen
    base, pen mask and shift, texture bank, fog mix (255 = none) and colour,
    fade mix and colour, polygon fade colour, alpha mix and pen, the clip
    rectangle (min x, min y, max x + 1, max y + 1 in target pixels), sprite
    source offset, line modulo and flip, and the priority bit.

***************************************************************************/
#ifndef MAME_NAMCO_NAMCOS22_SHADER_H
#define MAME_NAMCO_NAMCOS22_SHADER_H

#pragma once

constexpr int NAMCOS22_SHADER_STATE_WORDS = 28;
constexpr int NAMCOS22_SHADER_VARYINGS = 4;

constexpr char NAMCOS22_FRAGMENT_SOURCE[] =
	"#define S_FLAGS 0\n"
	"#define S_PENBASE 1\n"
	"#define S_PENMASK 2\n"
	"#define S_PENSHIFT 3\n"
	"#define S_BANK 4\n"
	"#define S_FOGMIX 5\n"
	"#define S_FOGR 6\n"
	"#define S_FOGG 7\n"
	"#define S_FOGB 8\n"
	"#define S_FADEMIX 9\n"
	"#define S_FADER 10\n"
	"#define S_FADEG 11\n"
	"#define S_FADEB 12\n"
	"#define S_POLYR 13\n"
	"#define S_POLYG 14\n"
	"#define S_POLYB 15\n"
	"#define S_ALPHAMIX 16\n"
	"#define S_ALPHAPEN 17\n"
	"#define S_CLIP 18\n"
	"#define S_SPRITE 22\n"
	"#define S_LINE 23\n"
	"#define S_FLIP 24\n"
	"#define S_PRIO 25\n"
	"#define F_SPRITE 1\n"
	"#define F_TEXTURE 2\n"
	"#define F_SHADE 4\n"
	"#define F_SUPER 8\n"
	"#define F_PFADE 16\n"
	"#define F_ALPHA 32\n"
	"#define F_STAMP 64\n"
	"#define F_FILTER 128\n"
	"ivec3 mix_color(ivec3 a, ivec3 b, int factor)\n"
	"{\n"
	"	return (a * factor + b * (256 - factor)) >> 8;\n"
	"}\n"
	"ivec3 palette_rgb(int index)\n"
	"{\n"
	"	return ivec3(texelFetch(palette, ivec2(index & 255, index >> 8), 0).zyx * 255.0 + 0.5);\n"
	"}\n"
	"int tile_pixel(int tx, int ty)\n"
	"{\n"
	"	int to = ((ty << 4) & 0xfff00) | (tx >> 4);\n"
	"	int tile = int(texelFetch(tile_map, ivec2(to & 1023, to >> 10), 0).r);\n"
	"	int attr = int(texelFetch(tile_attr, ivec2(to & 1023, to >> 10), 0).r);\n"
	"	int ix = tx & 15;\n"
	"	int iy = ty & 15;\n"
	"	if ((attr & 4) != 0)\n"
	"		ix = 15 - ix;\n"
	"	if ((attr & 2) != 0)\n"
	"		iy = 15 - iy;\n"
	"	if ((attr & 8) != 0)\n"
	"	{\n"
	"		int t = ix;\n"
	"		ix = iy;\n"
	"		iy = t;\n"
	"	}\n"
	"	int offset = (tile << 8) | (iy << 4) | ix;\n"
	"	return int(texelFetch(tile_data, ivec2(offset & 4095, offset >> 12), 0).r);\n"
	"}\n"
	"ivec3 apply_fog(ivec3 rgb)\n"
	"{\n"
	"	if (state(S_FOGMIX) != 255)\n"
	"		return mix_color(rgb, ivec3(state(S_FOGR), state(S_FOGG), state(S_FOGB)), state(S_FOGMIX));\n"
	"	return rgb;\n"
	"}\n"
	"ivec3 apply_fade(ivec3 rgb)\n"
	"{\n"
	"	if (state(S_FADEMIX) != 255)\n"
	"		return mix_color(rgb, ivec3(state(S_FADER), state(S_FADEG), state(S_FADEB)), state(S_FADEMIX));\n"
	"	return rgb;\n"
	"}\n"
	"ivec3 tap_rgb(int tx, int ty)\n"
	"{\n"
	"	int pen = tile_pixel(tx & 0xfff, (ty & 0xfff) | state(S_BANK));\n"
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
	"		color = vec4(texelFetch(picture, p / max(state(S_SPRITE), 1), 0).zyx, 0.0);\n"
	"		return true;\n"
	"	}\n"
	"	int pen = 0;\n"
	"	ivec3 rgb;\n"
	"	if ((flags & F_SPRITE) != 0)\n"
	"	{\n"
	"		int fx = state(S_FLIP) & 1;\n"
	"		int fy = (state(S_FLIP) >> 1) & 1;\n"
	"		int xi = clamp(int(v0 - float(fx)), 0, 31);\n"
	"		int yi = clamp(int(v1 - float(fy)), 0, 31);\n"
	"		int offset = state(S_SPRITE) + yi * state(S_LINE) + xi;\n"
	"		pen = int(texelFetch(sprite_data, ivec2(offset & 4095, offset >> 12), 0).r);\n"
	"		if (pen == 255)\n"
	"			return false;\n"
	"		rgb = apply_fade(apply_fog(palette_rgb(state(S_PENBASE) + pen)));\n"
	"	}\n"
	"	else\n"
	"	{\n"
	"		float ooz = 1.0 / v0;\n"
	"		if ((flags & F_TEXTURE) != 0)\n"
	"		{\n"
	"			int tx = int(v1 * ooz) & 0xfff;\n"
	"			int ty = (int(v2 * ooz) & 0xfff) | state(S_BANK);\n"
	"			pen = tile_pixel(tx, ty);\n"
	"		}\n"
	"		rgb = palette_rgb(state(S_PENBASE) + ((pen >> state(S_PENSHIFT)) & state(S_PENMASK)));\n"
	"		if ((flags & (F_TEXTURE | F_FILTER)) == (F_TEXTURE | F_FILTER))\n"
	"			rgb = filtered_rgb(v1 * ooz, v2 * ooz);\n"
	"#if ANISO_MAX > 1\n"
	"		if ((flags & F_TEXTURE) != 0)\n"
	"		{\n"
	"			vec2 axis;\n"
	"			int taps = aniso_taps(uv_dx, uv_dy, ANISO_MAX, axis);\n"
	"			if (taps > 1)\n"
	"			{\n"
	"				ivec3 sum = ivec3(0);\n"
	"				vec2 uv = vec2(v1, v2) * ooz;\n"
	"				for (int i = 0; i < taps; i++)\n"
	"				{\n"
	"					vec2 o = axis * ((float(i) + 0.5) / float(taps) - 0.5);\n"
	"					if ((flags & F_FILTER) != 0)\n"
	"						sum += filtered_rgb(uv.x + o.x, uv.y + o.y);\n"
	"					else\n"
	"						sum += tap_rgb(int(uv.x + o.x), int(uv.y + o.y));\n"
	"				}\n"
	"				rgb = (sum + ivec3(taps / 2)) / taps;\n"
	"			}\n"
	"		}\n"
	"#endif\n"
	"		int shade_level = ((flags & F_SHADE) != 0) ? int(v3 * ooz) : 0;\n"
	"		if ((flags & F_SUPER) == 0)\n"
	"		{\n"
	"			rgb = apply_fog(rgb);\n"
	"			if ((flags & F_SHADE) != 0)\n"
	"				rgb = clamp((rgb * (shade_level << 2)) >> 8, 0, 255);\n"
	"		}\n"
	"		else\n"
	"		{\n"
	"			if ((flags & F_SHADE) != 0)\n"
	"				rgb = clamp((rgb * (shade_level << 2)) >> 8, 0, 255);\n"
	"			rgb = apply_fog(rgb);\n"
	"			if ((flags & F_PFADE) != 0)\n"
	"				rgb = clamp((rgb * ivec3(state(S_POLYR), state(S_POLYG), state(S_POLYB))) >> 8, 0, 255);\n"
	"			rgb = apply_fade(rgb);\n"
	"		}\n"
	"	}\n"
	"	if ((flags & F_SUPER) != 0 && state(S_ALPHAMIX) != 255 && ((flags & F_ALPHA) != 0 || pen == state(S_ALPHAPEN)))\n"
	"		blend_factor = float(state(S_ALPHAMIX)) / 256.0;\n"
	"	color = vec4(vec3(rgb) / 255.0, (state(S_PRIO) != 0) ? 1.0 : 128.0 / 255.0);\n"
	"	return true;\n"
	"}\n";

#endif // MAME_NAMCO_NAMCOS22_SHADER_H
