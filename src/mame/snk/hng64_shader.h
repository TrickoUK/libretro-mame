// license:BSD-3-Clause
// copyright-holders:TrickoUK
/***************************************************************************

    hng64_shader.h

    Pixel pipeline of the SNK Hyper Neo Geo 64 host-GPU path, as a
    gpu_shader_module (see osd/interface/gpuengine.h). It evaluates the
    same per-pixel steps as hng64_poly_renderer::render_texture_scanline()
    and render_flat_scanline(): texture scroll, small page wrapping, 4 or
    8 bits per pixel texel fetch and the packed colour word
    (llll appp pppp pppp: lighting, blend flag, palette index).

    The colour word is written, not a colour: the red channel holds its low
    byte and the green channel its high byte, so the frame buffer read back
    is the same 16-bit index buffer the software renderer fills, and the
    palette, fade and blend resolve stay in the compositor.

    Binding: "tex_rom" (R8UI, 1024 wide, one 1024 x 1024 page after another).

    Varyings (screen-linear): v0 = 1/w, v1 = light/w, v2 = s/w, v3 = t/w.
    Depth is the window z.

    Primitive state record, 12 words: flags (bit 0 flat shaded, bit 1 4bpp
    texture, bit 2 blend), texture page, palette offset, small page
    register, flat colour index, texture scroll x and y, texture mask x
    and y.

***************************************************************************/
#ifndef MAME_SNK_HNG64_SHADER_H
#define MAME_SNK_HNG64_SHADER_H

#pragma once

constexpr int HNG64_SHADER_STATE_WORDS = 12;
constexpr int HNG64_SHADER_VARYINGS = 4;

constexpr char HNG64_FRAGMENT_SOURCE[] =
	"float cfmod(float x, float y) { return x - y * trunc(x / y); }\n"
	"bool shade(out vec4 color) {\n"
	"    int flags = state(0);\n"
	"    int word;\n"
	"    if ((flags & 1) != 0) {\n"
	"        word = (state(2) + state(4)) & 65535;\n"
	"    } else {\n"
	"        int page = state(1), pal = state(2), spage = state(3);\n"
	"        float w = v0;\n"
	"        float ts = (v2 / w) * 1024.0 + float((state(6) & 16383) >> 5);\n"
	"        float tt = (v3 / w) * 1024.0 + float((state(5) & 16383) >> 5);\n"
	"        if ((((spage & 49152) >> 14) & 2) != 0) {\n"
	"            tt = cfmod(tt, float(state(7))) + 8.0 * float((spage & 16256) >> 7);\n"
	"            ts = cfmod(ts, float(state(8))) + 8.0 * float(spage & 127);\n"
	"        }\n"
	"        int t = int(tt) & 1023, s = int(ts) & 1023;\n"
	"        int index;\n"
	"        if ((flags & 2) != 0) index = s * 512 + (t >> 1); else index = s * 1024 + t;\n"
	"        int entry = int(texelFetch(tex_rom, ivec2(index & 1023, page * 1024 + (index >> 10)), 0).r);\n"
	"        if ((flags & 2) != 0) entry = ((t & 1) != 0) ? ((entry >> 4) & 15) : (entry & 15);\n"
	"        if (entry == 0) return false;\n"
	"        int lightval = int((v1 / w) / 16.0) & 255;\n"
	"        word = ((pal + entry) & 2047) | (lightval << 12);\n"
	"        if ((flags & 4) != 0) word |= 2048;\n"
	"        word &= 65535;\n"
	"    }\n"
	"    color = vec4(float(word & 255) / 255.0, float(word >> 8) / 255.0, 0.0, 1.0);\n"
	"    return true;\n"
	"}\n";

#endif // MAME_SNK_HNG64_SHADER_H
