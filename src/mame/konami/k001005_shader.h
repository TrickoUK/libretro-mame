// license:BSD-3-Clause
// copyright-holders:TrickoUK
/***************************************************************************

    k001005_shader.h

    Pixel pipeline of the Konami K001005 host-GPU path, as a
    gpu_shader_module (see osd/interface/gpuengine.h). It evaluates the
    same per-pixel steps as k001005_renderer::draw_scanline_generic():
    8-bit palettised texel fetch with optional mirroring and bilinear
    filtering, diffuse and ambient lighting, linear fog and the blend
    factor used against the frame buffer.

    Bindings: "tex_rom" (R8UI, 512 wide, one 512 x 512 page after another),
    "palette" (R32UI, 256 x 8, the K001006 pens as 0xaarrggbb).

    Varyings (screen-linear): v0 = diffuse intensity (0-255), v1 = fog
    factor (0-255), v2 = u/z, v3 = v/z, v4 = 1/z, v5-v8 = vertex red,
    green, blue and alpha (0-255).

    Primitive state record, 16 words: flags (bit 0 textured, bit 1 vertex
    colour, bit 2 fog, bit 3 bilinear, bit 4 texture mirroring, bit 5
    frame buffer blend), texture X and Y, texture width and height (in
    units of 8 texels, minus 1), texture page, palette base, diffuse light,
    ambient light and fog colour and polygon colour (0xaarrggbb), then the
    clip rectangle (min x, min y, max x, max y, inclusive, in target
    pixels).

    The colour written has the blend factor in alpha; the draw command
    blends with SRC_ALPHA / ONE_MINUS_SRC_ALPHA when the polygon blends
    with the frame buffer, otherwise alpha is 1.

***************************************************************************/
#ifndef MAME_KONAMI_K001005_SHADER_H
#define MAME_KONAMI_K001005_SHADER_H

#pragma once

constexpr int K001005_SHADER_STATE_WORDS = 16;
constexpr int K001005_SHADER_VARYINGS = 9;

constexpr char K001005_FRAGMENT_SOURCE[] =
	"ivec4 unpack(int w) { return ivec4((w >> 16) & 255, (w >> 8) & 255, w & 255, (w >> 24) & 255); }\n"
	"int mirror_index(int i, int k, bool mirror) {\n"
	"    int size = (k + 1) * 8;\n"
	"    if (!mirror) return i % size;\n"
	"    int m = i % (size * 2);\n"
	"    return (m >= size) ? ((size - 1) - (i % size)) : (i % size);\n"
	"}\n"
	// x = red, y = green, z = blue, w = alpha
	"ivec4 fetch_texel(int page, int pal_base, int u, int v) {\n"
	"    uint idx = texelFetch(tex_rom, ivec2(u & 511, page * 512 + (v & 511)), 0).r;\n"
	"    if (idx == 0u) return ivec4(0);\n"
	"    int pen = pal_base + int(idx);\n"
	"    return unpack(int(texelFetch(palette, ivec2(pen & 255, pen >> 8), 0).r));\n"
	"}\n"
	"ivec4 scale8(ivec4 c, ivec4 s) { return clamp((c * s) >> 8, 0, 255); }\n"
	"ivec4 blend8(ivec4 c, ivec4 other, int f) { return (c * f + other * (256 - f)) >> 8; }\n"
	"bool shade(out vec4 color) {\n"
	"    ivec2 pos = ivec2(frag_pos());\n"
	"    if (pos.x < state(11) || pos.y < state(12) || pos.x > state(13) || pos.y > state(14)) return false;\n"
	"    int flags = state(0);\n"
	"    bool textured = (flags & 1) != 0;\n"
	"    bool vertex_color = (flags & 2) != 0;\n"
	"    bool use_fog = (flags & 4) != 0;\n"
	"    bool bilinear = (flags & 8) != 0;\n"
	"    bool mirror = (flags & 16) != 0;\n"
	"    bool fb_blend = (flags & 32) != 0;\n"
	"    int idiff = clamp(int(v0), 0, 255);\n"
	"    int ifog = clamp(int(v1), 0, 255);\n"
	"    ivec4 light = scale8(unpack(state(7)), ivec4(idiff));\n"
	"    light = clamp(light + unpack(state(8)), 0, 255);\n"
	"    ivec4 texel = ivec4(0);\n"
	"    int texel_alpha = 0;\n"
	"    if (textured) {\n"
	"        int tx = state(1), ty = state(2), tw = state(3), th = state(4), page = state(5), pal = state(6);\n"
	"        float oow = 1.0 / v4;\n"
	"        int iu = int(v2 * oow), iv = int(v3 * oow);\n"
	"        if (!bilinear) {\n"
	"            int tu = tx + mirror_index((iu >> 4) & 127, tw, mirror);\n"
	"            int tv = ty + mirror_index((iv >> 4) & 127, th, mirror);\n"
	"            texel = fetch_texel(page, pal, tu, tv);\n"
	"            texel_alpha = texel.w;\n"
	"        } else {\n"
	// sub-texel bias to avoid seams
	"            iu -= 7; iv -= 7;\n"
	"            int uf = iu & 15, vf = iv & 15;\n"
	"            int u0 = tx + mirror_index((iu >> 4) & 127, tw, mirror);\n"
	"            int u1 = tx + mirror_index(((iu >> 4) + 1) & 127, tw, mirror);\n"
	"            int v0i = ty + mirror_index((iv >> 4) & 127, th, mirror);\n"
	"            int v1i = ty + mirror_index(((iv >> 4) + 1) & 127, th, mirror);\n"
	"            ivec4 t00 = fetch_texel(page, pal, u0, v0i), t01 = fetch_texel(page, pal, u1, v0i);\n"
	"            ivec4 t10 = fetch_texel(page, pal, u0, v1i), t11 = fetch_texel(page, pal, u1, v1i);\n"
	// cut-outs keep the silhouette of the unfiltered texture: transparency comes from the nearest texel,
	// and transparent neighbours borrow its colour
	"            ivec4 nearest = t00;\n"
	"            if (vf >= 9 && uf >= 9) nearest = t11; else if (vf >= 9) nearest = t10; else if (uf >= 9) nearest = t01;\n"
	"            texel_alpha = nearest.w;\n"
	"            if (texel_alpha != 0) {\n"
	"                if (t00.w == 0) t00 = nearest;\n"
	"                if (t01.w == 0) t01 = nearest;\n"
	"                if (t10.w == 0) t10 = nearest;\n"
	"                if (t11.w == 0) t11 = nearest;\n"
	"            }\n"
	"            int fu = uf * 16, fv = vf * 16;\n"
	"            ivec4 r0 = t00 * (256 - fu) + t01 * fu;\n"
	"            ivec4 r1 = t10 * (256 - fu) + t11 * fu;\n"
	"            texel = (r0 * (256 - fv) + r1 * fv) >> 16;\n"
	"        }\n"
	"    }\n"
	"    ivec4 c;\n"
	"    int factor = 255;\n"
	"    if (vertex_color) {\n"
	"        int ia = clamp(int(v8), 0, 255);\n"
	"        if (ia == 0) return false;\n"
	"        c = ivec4(clamp(int(v5), 0, 255), clamp(int(v6), 0, 255), clamp(int(v7), 0, 255), ia);\n"
	"        if (textured && texel_alpha != 0) c = texel;\n"
	"        factor = ia;\n"
	"    } else if (textured) {\n"
	"        if (texel_alpha == 0) return false;\n"
	"        c = texel;\n"
	"        factor = (state(10) >> 24) & 255;\n"
	"    } else {\n"
	"        c = unpack(state(10));\n"
	"        factor = (state(10) >> 24) & 255;\n"
	"    }\n"
	"    c = scale8(c, light);\n"
	"    if (use_fog) c = blend8(c, unpack(state(9)), ifog);\n"
	"    color = vec4(vec3(c.xyz) / 255.0, fb_blend ? float(factor) / 255.0 : 1.0);\n"
	"    return true;\n"
	"}\n";

#endif // MAME_KONAMI_K001005_SHADER_H
