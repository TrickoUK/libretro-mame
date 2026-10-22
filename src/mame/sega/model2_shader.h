// license:BSD-3-Clause
// copyright-holders:TrickoUK
/***************************************************************************

    model2_shader.h

    Pixel pipeline of the Sega Model 2 host-GPU path, as a gpu_shader_module
    (see osd/interface/gpuengine.h). It evaluates the same per-pixel steps as
    the software scanline routines in model2rd.ipp: a 4bpp texel fetch from
    the two texture sheets, fixed-point bilinear filtering with mip level
    blending and microtexture, then the luma ramp, the three colour tables
    and the gamma ramp.

    Bindings: "tex_sheets" (R16UI, 512 x 2048, texture sheet 0 above sheet
    1, a 16-bit word holds a 2x2 block of texels), "luma_ram" (R8UI, 256 x
    128), "color_xlat" (R16UI, 256 x 96), "gamma_ramp" (R8UI, 256 x 1).

    Varyings (screen-linear): v0 = 1/z, v1 = u/z, v2 = v/z.

    Primitive state record, 20 words: kind (0 solid, 1 translucent solid,
    2 textured, 3 translucent textured) with bit 8 checker, bits 9-12 wrap
    and mirror, bit 13 primary sheet, bit 15 microtexture, bits 16-17
    microtexture minimum LOD; texture LOD; luma; texture width, height,
    X and Y; microtexture X and Y; luma table base; the three colour table
    bases; the viewport rectangle (min x, min y, max x, max y).

***************************************************************************/
#ifndef MAME_SEGA_MODEL2_SHADER_H
#define MAME_SEGA_MODEL2_SHADER_H

#pragma once

constexpr int MODEL2_SHADER_STATE_WORDS = 20;
constexpr int MODEL2_SHADER_VARYINGS = 3;

constexpr char MODEL2_FRAGMENT_SOURCE[] =
	"ivec4 P0, P1, P2, P3, P4;\n"
	"int g_sheet;\n"
	"uint get_texel(int sheet, int bx, int by, int x, int y) {\n"
	"    int x2 = bx + x, y2 = by + y;\n"
	"    if (x2 >= 1024) { x2 -= 1024; y2 ^= 1024; }\n"
	"    int offset = (y2 / 2) * 512 + (x2 / 2);\n"
	"    uint w = texelFetch(tex_sheets, ivec2(offset & 511, (offset >> 9) + sheet * 1024), 0).r;\n"
	"    if ((y & 1) == 0) w >>= 8;\n"
	"    if ((x & 1) == 0) w >>= 4;\n"
	"    return w & 15u;\n"
	"}\n"
	"ivec2 lerp2(ivec2 a, ivec2 b, int f) { return (a + (((b - a) * f) >> 8)) & 255; }\n"
	// x = luma byte, y = alpha (0 or 0x80)
	"ivec2 fetch_bilinear(bool transl, int miplevel, int u, int v) {\n"
	"    int wrapx = (P0.x >> 9) & 1, wrapy = (P0.x >> 10) & 1, mirx = (P0.x >> 11) & 1, miry = (P0.x >> 12) & 1;\n"
	"    int tw, th, tx, ty, sheet;\n"
	"    if (miplevel == -1) {\n"
	"        tw = 128; th = 128; tx = P2.x; ty = P2.y; sheet = g_sheet ^ 1;\n"
	"        int sh = 1 << ((P0.x >> 16) & 3);\n"
	"        u <<= sh; v <<= sh;\n"
	"    } else {\n"
	"        tw = P1.x >> miplevel; th = P1.y >> miplevel;\n"
	"        tx = ((P1.z - 2048) >> miplevel) & 2047; ty = ((P1.w - 1024) >> miplevel) & 1023;\n"
	"        sheet = g_sheet ^ (miplevel & 1);\n"
	"        u >>= miplevel; v >>= miplevel;\n"
	"    }\n"
	"    if (mirx != 0 && (u & (tw << 8)) != 0) u = ~u;\n"
	"    if (miry != 0 && (v & (th << 8)) != 0) v = ~v;\n"
	"    u -= 0x80; v -= 0x80;\n"
	"    int ufrac = u & 0xff, vfrac = v & 0xff;\n"
	"    int u0 = (u >> 8) & (tw - 1), u1 = (u0 + 1) & (tw - 1);\n"
	"    int v0 = (v >> 8) & (th - 1), v1 = (v0 + 1) & (th - 1);\n"
	"    if (wrapx == 0 && u1 == 0) {\n"
	"        if (ufrac >= 0x80) { u0 = u1; u1++; ufrac = 0; } else { u1 = u0; u0--; ufrac = 0x100; }\n"
	"    }\n"
	"    if (wrapy == 0 && v1 == 0) {\n"
	"        if (vfrac >= 0x80) { v0 = 0; v1++; vfrac = 0; } else { v1 = v0; v0--; vfrac = 0x100; }\n"
	"    }\n"
	"    ivec2 t00 = ivec2(int(get_texel(sheet, tx, ty, u0, v0)) << 4, 0);\n"
	"    ivec2 t01 = ivec2(int(get_texel(sheet, tx, ty, u1, v0)) << 4, 0);\n"
	"    ivec2 t10 = ivec2(int(get_texel(sheet, tx, ty, u0, v1)) << 4, 0);\n"
	"    ivec2 t11 = ivec2(int(get_texel(sheet, tx, ty, u1, v1)) << 4, 0);\n"
	"    if (transl) {\n"
	"        if (t00.x != 0xf0) t00.y = 0x80;\n"
	"        if (t01.x != 0xf0) t01.y = 0x80;\n"
	"        if (t10.x != 0xf0) t10.y = 0x80;\n"
	"        if (t11.x != 0xf0) t11.y = 0x80;\n"
	"        if (t00 == ivec2(0xf0, 0)) t00 = ivec2(t01.x, 0);\n"
	"        if (t01 == ivec2(0xf0, 0)) t01 = ivec2(t00.x, 0);\n"
	"        if (t10 == ivec2(0xf0, 0)) t10 = ivec2(t11.x, 0);\n"
	"        if (t11 == ivec2(0xf0, 0)) t11 = ivec2(t10.x, 0);\n"
	"    }\n"
	"    ivec2 t0x = lerp2(t00, t01, ufrac);\n"
	"    ivec2 t1x = lerp2(t10, t11, ufrac);\n"
	"    if (transl) {\n"
	"        if (t0x == ivec2(0xf0, 0)) t0x = ivec2(t1x.x, 0);\n"
	"        if (t1x == ivec2(0xf0, 0)) t1x = ivec2(t0x.x, 0);\n"
	"    }\n"
	"    return lerp2(t0x, t1x, vfrac);\n"
	"}\n"
	"bool shade(out vec4 color) {\n"
	"    P0 = ivec4(state(0), state(1), state(2), state(3));\n"
	"    P1 = ivec4(state(4), state(5), state(6), state(7));\n"
	"    P2 = ivec4(state(8), state(9), state(10), state(11));\n"
	"    P3 = ivec4(state(12), state(13), state(14), state(15));\n"
	"    P4 = ivec4(state(16), state(17), state(18), state(19));\n"
	"    ivec2 pos = ivec2(frag_pos());\n"
	"    int sx = pos.x, sy = pos.y;\n"
	"    if (sx < P4.x || sx > P4.z || sy < P4.y || sy > P4.w) return false;\n"
	"    int kind = P0.x & 3;\n"
	"    if (((P0.x >> 8) & 1) != 0 && (((sx ^ sy) & 1) == 0)) return false;\n"
	"    int luma;\n"
	"    if (kind < 2) {\n"
	"        if (kind == 1) return false;\n"      // translucent solid: nothing drawn
	"        luma = P0.z >> 2;\n"
	"    } else {\n"
	"        bool transl = kind == 3;\n"
	"        g_sheet = (P0.x >> 13) & 1;\n"
	"        float z = 1.0 / v0;\n"
	"        int maxlevel = int(floor(log2(float(min(P1.x, P1.y))) + 0.5)) - 1;\n"
	"        int mml = int(floor(log2(z) * 256.0)) - P0.y;\n"
	"        int level = clamp(mml >> 7, 0, maxlevel);\n"
	"        int u = int(v1 * z * 256.0), v = int(v2 * z * 256.0);\n"
	"        ivec2 t = fetch_bilinear(transl, level, u, v);\n"
	"        if (mml > 0 && level < maxlevel) {\n"
	"            ivec2 t2 = fetch_bilinear(transl, level + 1, u, v);\n"
	"            t = lerp2(t, t2, (mml & 127) << 1);\n"
	"        } else if (((P0.x >> 15) & 1) != 0 && mml < 0) {\n"
	"            ivec2 t2 = fetch_bilinear(transl, -1, u, v);\n"
	"            t = lerp2(t, t2, min(-mml >> ((P0.x >> 16) & 3), 127));\n"
	"        }\n"
	"        if (transl && t.y < 0x40) return false;\n"
	"        int li = P2.z + (t.x >> 1);\n"
	"        int lr = int(texelFetch(luma_ram, ivec2(li & 255, li >> 8), 0).r);\n"
	"        luma = min(lr * P0.z / 256, 0x3f);\n"
	"    }\n"
	"    int ri = P3.x + luma, gi = P3.y + luma, bi = P3.z + luma;\n"
	"    int tr = int(texelFetch(color_xlat, ivec2(ri & 255, ri >> 8), 0).r) & 255;\n"
	"    int tg = int(texelFetch(color_xlat, ivec2(gi & 255, gi >> 8), 0).r) & 255;\n"
	"    int tb = int(texelFetch(color_xlat, ivec2(bi & 255, bi >> 8), 0).r) & 255;\n"
	"    float r = float(texelFetch(gamma_ramp, ivec2(tr, 0), 0).r) / 255.0;\n"
	"    float g = float(texelFetch(gamma_ramp, ivec2(tg, 0), 0).r) / 255.0;\n"
	"    float b = float(texelFetch(gamma_ramp, ivec2(tb, 0), 0).r) / 255.0;\n"
	"    color = vec4(r, g, b, 1.0);\n"
	"    return true;\n"
	"}\n";

#endif // MAME_SEGA_MODEL2_SHADER_H
