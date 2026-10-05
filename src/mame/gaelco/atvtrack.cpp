// license:BSD-3-Clause
// copyright-holders:Samuele Zannoli
/*
  ATV Track
  (c)2002 Gaelco

ATV Track
Gaelco 2002

PCB Layout

GAELCO
REF. 020419
 |--------------------------------------------------------------|
 |                                                              |
 |   SW3                                               EPC1PC8  |
 |                                         K4S643232            |
 |    LC245A                                         7LB176    |-|
 |                        FLASH.IC14  FLASH.IC19     7LB176    | |
 |       |-----|                                     7LB176    | |DB9
 |       | SH4 |                                     7LB176    |-|
 |       |     |          FLASH.IC15  FLASH.IC20                |
 |       |-----|                                                |
 |                                                |----------|  |
|-|                            K4S643232          |ALTERA    |  |
| |  L4955                                        |FLEX      |  |
| |      |-----|               K4S643232          |EPF10K50  |  |
| |CN1   | SH4 |                                  |EQC240-3  |  |
| |      |     |                                  |----------|  |
| |      |-----|                                                |
|-|                                                             |
 |                                                              |
 |      33MHz                       K4S643232     |----------|  |
 |                                  K4S643232     | GFX      |  |
 |             LED                                |          | |-|
 |             LED                                |          | | |
 |                                  K4S643232     |          | | |DB9
 |                                  K4S643232     |----------| |-|
 | TL074C   TL074C                                     385-1    |
 |     TDA1387   TDA1387                          14.31818MHz   |
 |                                                              |
 |--------------------------------------------------------------|
Notes:
      SH4       - Hitachi HD6417750S SH4 CPU (BGA)
      K4S643232 - Samsung K4S643232E-TC70 64M x 32-bit SDRAM (TSSOP86)
      GFX       - NEC PowerVR Neon 250
      FLASH.IC* - Samsung K9F2808U0B 128MBit (16M + 512k Spare x 8-bit) FlashROM (TSOP48)
      EPF10K50  - Altera Flex EPF10K50EQC240-3 FPGA (QFP240)
      EPC1PC8   - Altera EPC1PC8 FPGA Configuration Device (DIP8)
      TL074C    - Texas Instruments TL074C Low Noise Quad JFet Operational Amplifier (SOIC14)
      TDA1387   - Philips TDA1387 Stereo Continuous Calibration DAC (SOIC8)
      L4955     - ST Microelectronics L4955 low-power, quad channel, 8-bit buffered voltage output DAC and amplifier
      7LB176    - Texas Instruments 7LB176 Differential Bus Tranceiver (SOIC8)
      385-1     - National LM385 Adjustable Micropower Voltage Reference Diode (SOIC8)
      CN1       - Multi-pin connector for filter board (input, video, power & controls connectors etc)
      DB9       - Probably used for cabinet linking
      SW3       - Push button switch


Gaelco Football
(c) 2002 Gaelco

PCB:
GAELCO
REF. 020201
Same PCB as above ATV Track, except for HD6417750 SH4 CPUs was used instead of HD6417750S.

*/

/*

notes from DEMUL team

Smashing Drive needs a working SH4 MMU emulation, ATV Track does not.

Audio - is a simple buffered DAC.
frequency is 32kHz
data written by CPU to buffer have such meaning:
offs 0 - s16 bass channel 0
offs 2 - s16 bass channel 1
offs 4 - s16 left channel
offs 6 - s16 right channel
and so on

buffer is 2x32bytes
then it becomes (I suppose half) empty - SH4 IRL5 IRQ generated


"control registers" (Smashing Drive)
0 - read - various statuses, returning -1 is OK
write - enable slave CPU, gpu, etc most of bits is unclear
4 - w - RS422/485 communication port (for cabinet linking)

SH4 XTAL is 33MHz, SH4 MD0-2 pins is 001 or 011 (CPU core clk = XTAL*6, preipheral clk = XTAL, bus clk is XTAL or XTAL*2)

TODO:
    devicify NAND
    somehow hook PVR2 renderer here
    add sound

*/

#include "emu.h"
#include "cpu/sh/sh4.h"
#include "debugger.h"
#include "machine/adc083x.h"
#include "emupal.h"
#include "screen.h"
#include "speaker.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <thread>
#include <vector>



DECLARE_DEVICE_TYPE(GAELCO_ATVTRACK_DAC, gaelco_atvtrack_dac_device)

// Buffered stereo DAC: a 64 byte double buffer on the main CPU's bus, two halves of four 8 byte frames (bass 0, bass 1,
// left, right, signed 16 bit each) played at 32kHz. Every time a half has been played the DAC raises the main CPU's IRL
// interrupt until the half is written again, which is when the game's software mixer refills it.
class gaelco_atvtrack_dac_device : public device_t, public device_sound_interface
{
public:
	gaelco_atvtrack_dac_device(const machine_config &mconfig, const char *tag, device_t *owner, u32 clock = 0)
		: device_t(mconfig, GAELCO_ATVTRACK_DAC, tag, owner, clock)
		, device_sound_interface(mconfig, *this)
		, m_irq_cb(*this)
	{
	}

	auto irq_cb() { return m_irq_cb.bind(); }

	u64 buffer_r(offs_t offset) { return m_buf[offset & 7]; }
	void buffer_w(offs_t offset, u64 data, u64 mem_mask)
	{
		offset &= 7;
		COMBINE_DATA(&m_buf[offset]);
		if (!m_running)
		{
			m_running = true;
			m_timer->adjust(attotime::from_hz(SAMPLE_RATE / 4), 0, attotime::from_hz(SAMPLE_RATE / 4));
		}
		// writing a half acknowledges its interrupt
		const u8 bit = 1 << (offset >> 2);
		if (m_pending & bit)
		{
			m_pending &= ~bit;
			if (!m_pending)
				m_irq_cb(0);
		}
	}

protected:
	virtual void device_start() override ATTR_COLD
	{
		m_stream = stream_alloc(0, 2, SAMPLE_RATE);
		m_timer = timer_alloc(FUNC(gaelco_atvtrack_dac_device::tick), this);
		save_item(NAME(m_buf));
		save_item(NAME(m_half));
		save_item(NAME(m_pending));
		save_item(NAME(m_running));
	}

	virtual void device_reset() override ATTR_COLD
	{
		std::fill(std::begin(m_buf), std::end(m_buf), 0);
		m_half = 0;
		m_pending = 0;
		m_running = false;
		m_head = m_tail = 0;
		// the DAC free runs from power on: its buffer starts empty, so the interrupt is requested right away
		m_running = true;
		m_timer->adjust(attotime::from_hz(SAMPLE_RATE / 4), 0, attotime::from_hz(SAMPLE_RATE / 4));
		m_irq_cb(0);
	}

	virtual void sound_stream_update(sound_stream &stream) override
	{
		for (int i = 0; i < stream.samples(); i++)
		{
			if (m_head != m_tail)
			{
				stream.put_int(0, i, m_fifo[m_tail][0], 32768);
				stream.put_int(1, i, m_fifo[m_tail][1], 32768);
				m_tail = (m_tail + 1) % FIFO_SIZE;
			}
		}
	}

private:
	static constexpr int SAMPLE_RATE = 32000;
	static constexpr int FIFO_SIZE = 4096;

	TIMER_CALLBACK_MEMBER(tick)
	{
		// play the half that was just filled, then ask for it again
		m_stream->update();
		for (int f = 0; f < 4; f++)
		{
			const u64 frame = m_buf[m_half * 4 + f];
			const int next = (m_head + 1) % FIFO_SIZE;
			if (next != m_tail)
			{
				m_fifo[m_head][0] = s16(frame >> 32);
				m_fifo[m_head][1] = s16(frame >> 48);
				m_head = next;
			}
		}
		m_pending |= 1 << m_half;
		m_half ^= 1;
		m_irq_cb(1);
	}

	devcb_write_line m_irq_cb;
	sound_stream *m_stream = nullptr;
	emu_timer *m_timer = nullptr;
	u64 m_buf[8]{};
	u8 m_half = 0;
	u8 m_pending = 0;
	bool m_running = false;
	s16 m_fifo[FIFO_SIZE][2]{};
	int m_head = 0, m_tail = 0;
};

DEFINE_DEVICE_TYPE(GAELCO_ATVTRACK_DAC, gaelco_atvtrack_dac_device, "atvtrack_dac", "Gaelco ATV Track buffered DAC")

namespace {

//#define SPECIALMODE 1 // Alternate code path


// PowerVR "Neon 250" (PMX1) display list renderer, high level.
//
// The sub CPU's graphics library builds PowerVR2 style structures in the 32MB of GPU RAM: a context
// block pointing at page tables for up to 3 object lists, and the lists themselves: strip entries
// (bit 31 = a new 5 word state block follows: vertex base, vertex limit, ISP, TSP, texture control;
// bits 30-25 = triangle mask, bits 23-21 = vertex size - 3, bits 20-0 = first vertex in words),
// plus a vertex pool of screen space vertices (x, y, 1/w, u, v, ARGB). The ISP/TSP/texture words
// have the Dreamcast PVR2 layout, so the texture formats (twiddled, VQ, mipmapped) and the blend
// modes are the PVR2 ones too.
class neon250_renderer
{
public:
	static constexpr int WIDTH = 640;
	static constexpr int HEIGHT = 480;
	static constexpr u32 RAM_BASE = 0x18000000;
	static constexpr u32 RAM_MASK = 0x01ffffff;

	neon250_renderer()
	{
		for (int i = 0; i < 1024; i++)
		{
			u32 r = 0;
			for (int b = 0; b < 10; b++)
				r |= ((i >> b) & 1) << (2 * b);
			m_dil[i] = r;
		}
		m_depth.resize(WIDTH * HEIGHT);
	}

	void set_ram(const u32 *ram) { m_ram = ram; }
	void render(u32 ctx, bitmap_rgb32 &bitmap, u32 clear_color);

	u32 rd(u32 addr) const { return m_ram[(addr & RAM_MASK) >> 2]; }
	float rdf(u32 addr) const { u32 v = rd(addr); float f; memcpy(&f, &v, 4); return f; }

	int m_tris = 0, m_lists = 0;

private:
	struct vtx { float x, y, z, u, v; u32 col; };
	struct tex_level
	{
		int w = 0, h = 0;
		std::vector<u32> pix;
	};
	struct tex_entry
	{
		std::vector<tex_level> lv; // level 0 first
		u32 sig = 0;
		u32 frame = 0;
		bool valid() const { return !lv.empty(); }
	};

	struct tri_job
	{
		vtx v0, v1, v2;
		u32 isp, tsp;
		const tex_entry *tex;
	};
	std::vector<tri_job> m_queue;
	void rasterize(bitmap_rgb32 &bitmap);

	const u32 *m_ram = nullptr;
	u32 m_dil[1024];
	std::vector<float> m_depth;
	std::map<u64, tex_entry> m_texcache;
	u32 m_frame = 0;

	// PowerVR2 twiddling: x and y bits interleaved up to the smaller dimension, the rest of the larger one linear
	u32 tw_index(int x, int y, int wl, int hl) const
	{
		const int m = std::min(wl, hl);
		const u32 mask = (1u << m) - 1;
		u32 idx = (m_dil[x & mask] << 1) + m_dil[y & mask];
		if (wl > hl)
			idx += u32(x >> m) << (2 * m);
		else if (hl > wl)
			idx += u32(y >> m) << (2 * m);
		return idx;
	}
	u32 texture_signature(u32 base, u32 words) const;
	const tex_entry *get_texture(u32 tex, u32 tsp);
	void process_list(u32 pagetable, u32 words, bitmap_rgb32 &bitmap);
	void draw_triangle(const vtx &a, const vtx &b, const vtx &c, u32 isp, u32 tsp, const tex_entry *tex, bitmap_rgb32 &bitmap);
	void draw_triangle_raw(const vtx &a, const vtx &b, const vtx &c, u32 isp, u32 tsp, const tex_entry *tex, bitmap_rgb32 &bitmap, int part, int nparts);

	static u32 pf_to_argb(int pf, u16 v)
	{
		switch (pf)
		{
		case 0: // ARGB1555
			return ((v & 0x8000) ? 0xff000000 : 0) | (((v >> 10) & 31) * 255 / 31) << 16 | (((v >> 5) & 31) * 255 / 31) << 8 | ((v & 31) * 255 / 31);
		case 1: // RGB565
			return 0xff000000 | (((v >> 11) & 31) * 255 / 31) << 16 | (((v >> 5) & 63) * 255 / 63) << 8 | ((v & 31) * 255 / 31);
		case 2: // ARGB4444
			return (((v >> 12) & 15) * 17) << 24 | (((v >> 8) & 15) * 17) << 16 | (((v >> 4) & 15) * 17) << 8 | ((v & 15) * 17);
		default:
			return 0xffff00ff;
		}
	}
};

u32 neon250_renderer::texture_signature(u32 base, u32 words) const
{
	u32 sig = 0;
	u32 step = words > 64 ? words / 64 : 1;
	for (u32 i = 0; i < words; i += step)
		sig = sig * 31 + rd(base + i * 4);
	return sig;
}

const neon250_renderer::tex_entry *neon250_renderer::get_texture(u32 tex, u32 tsp)
{
	const int mip = (tex >> 31) & 1;
	const int vq = (tex >> 30) & 1;
	const int pf = (tex >> 27) & 7;
	const int scan = (tex >> 26) & 1;
	if (pf == 3 || pf == 4 || pf == 7)
		return nullptr; // YUV, bump map and reserved formats are not handled yet
	// formats 5 and 6 are 32 bit ARGB8888 on this board (the DC's palette formats), seen as cleared
	// render-target style buffers (alpha 0 fill)
	const bool argb32 = (pf == 5 || pf == 6);

	int su = (tsp >> 3) & 7;
	int sv = tsp & 7;
	if (mip)
		sv = su;
	const int w = 8 << su, h = 8 << sv;

	const u64 key = (u64(tex) << 8) | (tsp & 0x3f);
	tex_entry &te = m_texcache[key];
	const u32 base = RAM_BASE + (((tex & 0x1fffff) << 5) & RAM_MASK); // texture addresses are in 32 byte units
	const u32 sigwords = argb32 ? w * h : (vq ? 0x200 + (w * h) / 4 / 4 + (mip ? (w * h) / 12 : 0) : (w * h) / 2);
	const u32 sig = texture_signature(base, sigwords);
	if (te.valid() && te.sig == sig)
	{
		te.frame = m_frame;
		return &te;
	}

	te.lv.clear();
	te.sig = sig;
	te.frame = m_frame;

	auto rd16 = [this](u32 addr) -> u16 { return (rd(addr & ~3) >> ((addr & 2) * 8)) & 0xffff; };
	auto rd8 = [this](u32 addr) -> u8 { return (rd(addr & ~3) >> ((addr & 3) * 8)) & 0xff; };

	// level start offsets by size (1x1 .. 1024x1024), from the PowerVR2 mipmap layout
	static const u32 vqoff[11] = { 0x0, 0x1, 0x2, 0x6, 0x16, 0x56, 0x156, 0x556, 0x1556, 0x5556, 0x15556 };
	static const u32 npoff[11] = { 0x6, 0x8, 0x10, 0x30, 0xb0, 0x2b0, 0xab0, 0x2ab0, 0xaab0, 0x2aab0, 0xaaab0 };

	const int nlevels = mip ? (su + 1) : 1; // sizes 8<<su down to 8 (smaller levels are not worth sampling)
	for (int l = 0; l < nlevels; l++)
	{
		const int ls = su - l;        // log2(size/8) of this level
		const int lw = mip ? (8 << ls) : w;
		const int lh = mip ? (8 << ls) : h;
		tex_level lev;
		lev.w = lw;
		lev.h = lh;
		lev.pix.assign(lw * lh, 0xffff00ff);
		const int lg = ls + 3; // log2(size)
		const int wl = mip ? lg : su + 3, hl = mip ? lg : sv + 3; // log2 of the level's width/height
		if (argb32)
		{
			const u32 addr = base + (mip ? npoff[lg] * 2 : 0);
			for (int y = 0; y < lh; y++)
				for (int x = 0; x < lw; x++)
					lev.pix[y * lw + x] = rd(addr + tw_index(x, y, wl, hl) * 4);
		}
		else if (vq)
		{
			const u32 idxbase = base + 0x800 + (mip ? vqoff[lg] : 0);
			for (int y = 0; y < lh; y++)
				for (int x = 0; x < lw; x++)
				{
					const u32 idx = rd8(idxbase + (scan ? (u32(y >> 1) * (lw >> 1) + (x >> 1)) : tw_index(x >> 1, y >> 1, wl - 1, hl - 1)));
					lev.pix[y * lw + x] = pf_to_argb(pf, rd16(base + idx * 8 + (((m_dil[x & 1] << 1) + m_dil[y & 1]) * 2)));
				}
		}
		else if (!scan)
		{
			const u32 addr = base + (mip ? npoff[lg] : 0);
			for (int y = 0; y < lh; y++)
				for (int x = 0; x < lw; x++)
					lev.pix[y * lw + x] = pf_to_argb(pf, rd16(addr + tw_index(x, y, wl, hl) * 2));
		}
		else
		{
			for (int y = 0; y < lh; y++)
				for (int x = 0; x < lw; x++)
					lev.pix[y * lw + x] = pf_to_argb(pf, rd16(base + (y * lw + x) * 2)); // stride select not handled
		}
		te.lv.push_back(std::move(lev));
	}
	return &te;
}

void neon250_renderer::draw_triangle(const vtx &a, const vtx &b, const vtx &c, u32 isp, u32 tsp, const tex_entry *tex, bitmap_rgb32 &bitmap)
{
	// Triangles that stay near the screen go straight to the rasterizer. The game's geometry code leaves
	// clipping against the near plane to the hardware, so some vertices are millions of pixels away,
	// where the rasterizer's float edge functions have no precision left: clip those to a guard band
	// around the screen first, interpolating 1/w, u/w, v/w and the colour linearly in screen space.
	auto sane = [](const vtx &v) { return fabsf(v.x) < 3000.0f && fabsf(v.y) < 3000.0f; };
	if (sane(a) && sane(b) && sane(c))
	{
		m_queue.push_back({ a, b, c, isp, tsp, tex });
		return;
	}
	for (const vtx *v : { &a, &b, &c })
		if (!std::isfinite(v->x) || !std::isfinite(v->y) || !std::isfinite(v->z) || v->z <= 0.0f)
			return;

	struct cv { double x, y, z, uz, vz, ca, cr, cg, cb; };
	auto make = [](const vtx &v) { return cv{ v.x, v.y, v.z, double(v.u) * v.z, double(v.v) * v.z, double(v.col >> 24), double((v.col >> 16) & 255), double((v.col >> 8) & 255), double(v.col & 255) }; };
	std::vector<cv> poly = { make(a), make(b), make(c) };
	const double lo_x = -64, hi_x = WIDTH + 64, lo_y = -64, hi_y = HEIGHT + 64;
	for (int edge = 0; edge < 4 && poly.size() >= 3; edge++)
	{
		auto dist = [&](const cv &v) -> double
		{
			switch (edge)
			{
			case 0: return v.x - lo_x;
			case 1: return hi_x - v.x;
			case 2: return v.y - lo_y;
			default: return hi_y - v.y;
			}
		};
		std::vector<cv> out;
		for (size_t i = 0; i < poly.size(); i++)
		{
			const cv &p = poly[i], &q = poly[(i + 1) % poly.size()];
			const double dp = dist(p), dq = dist(q);
			if (dp >= 0)
				out.push_back(p);
			if ((dp >= 0) != (dq >= 0))
			{
				const double t = dp / (dp - dq);
				out.push_back(cv{ p.x + (q.x - p.x) * t, p.y + (q.y - p.y) * t, p.z + (q.z - p.z) * t, p.uz + (q.uz - p.uz) * t, p.vz + (q.vz - p.vz) * t,
						p.ca + (q.ca - p.ca) * t, p.cr + (q.cr - p.cr) * t, p.cg + (q.cg - p.cg) * t, p.cb + (q.cb - p.cb) * t });
			}
		}
		poly.swap(out);
	}
	if (poly.size() < 3)
		return;
	auto back = [](const cv &c) -> vtx
	{
		vtx v;
		v.x = float(c.x); v.y = float(c.y); v.z = float(c.z);
		v.u = c.z > 0 ? float(c.uz / c.z) : 0.0f;
		v.v = c.z > 0 ? float(c.vz / c.z) : 0.0f;
		v.col = (u32(std::clamp(c.ca, 0.0, 255.0)) << 24) | (u32(std::clamp(c.cr, 0.0, 255.0)) << 16) | (u32(std::clamp(c.cg, 0.0, 255.0)) << 8) | u32(std::clamp(c.cb, 0.0, 255.0));
		return v;
	};
	const vtx first = back(poly[0]);
	for (size_t i = 1; i + 1 < poly.size(); i++)
		m_queue.push_back({ first, back(poly[i]), back(poly[i + 1]), isp, tsp, tex });
}

void neon250_renderer::draw_triangle_raw(const vtx &v0, const vtx &v1, const vtx &v2, u32 isp, u32 tsp, const tex_entry *tex, bitmap_rgb32 &bitmap, int part, int nparts)
{
	const float area = (v1.x - v0.x) * (v2.y - v0.y) - (v2.x - v0.x) * (v1.y - v0.y);
	if (!(area > 1e-6f || area < -1e-6f))
		return;
	const int cull = (isp >> 27) & 3;
	if ((cull == 2 && area < 0) || (cull == 3 && area > 0))
		return;

	int minx = int(floorf(std::min({ v0.x, v1.x, v2.x })));
	int maxx = int(ceilf(std::max({ v0.x, v1.x, v2.x })));
	int miny = int(floorf(std::min({ v0.y, v1.y, v2.y })));
	int maxy = int(ceilf(std::max({ v0.y, v1.y, v2.y })));
	minx = std::max(minx, 0); miny = std::max(miny, 0);
	maxx = std::min(maxx, WIDTH - 1); maxy = std::min(maxy, HEIGHT - 1);
	if (minx > maxx || miny > maxy)
		return;

	const int depthmode = (isp >> 29) & 7;
	const bool zwrite = !((isp >> 26) & 1);
	const bool gouraud = (isp >> 23) & 1;
	const int srcinst = (tsp >> 29) & 7;
	const int dstinst = (tsp >> 26) & 7;
	const bool use_alpha = (tsp >> 20) & 1;
	const bool ignore_texa = (tsp >> 19) & 1;
	const int flipuv = (tsp >> 17) & 3;
	const int clampuv = (tsp >> 15) & 3;
	const int tsinst = (tsp >> 6) & 3;
	const float inv_area = 1.0f / area;
	const bool opaque_blend = (srcinst == 1 && dstinst == 0);
	const bool bilinear = ((tsp >> 13) & 3) != 0;
	const int nlev = tex ? int(tex->lv.size()) : 0;

	// barycentric gradients, and the gradients of the perspective numerators/denominator
	const float dl0dx = (v1.y - v2.y) * inv_area, dl0dy = (v2.x - v1.x) * inv_area;
	const float dl1dx = (v2.y - v0.y) * inv_area, dl1dy = (v0.x - v2.x) * inv_area;
	const float dl2dx = -dl0dx - dl1dx, dl2dy = -dl0dy - dl1dy;
	const float dWdx = dl0dx * v0.z + dl1dx * v1.z + dl2dx * v2.z, dWdy = dl0dy * v0.z + dl1dy * v1.z + dl2dy * v2.z;
	const float dUdx = dl0dx * v0.u * v0.z + dl1dx * v1.u * v1.z + dl2dx * v2.u * v2.z;
	const float dUdy = dl0dy * v0.u * v0.z + dl1dy * v1.u * v1.z + dl2dy * v2.u * v2.z;
	const float dVdx = dl0dx * v0.v * v0.z + dl1dx * v1.v * v1.z + dl2dx * v2.v * v2.z;
	const float dVdy = dl0dy * v0.v * v0.z + dl1dy * v1.v * v1.z + dl2dy * v2.v * v2.z;

	auto addr_uv = [](float t, int size, bool flip, bool clamp) -> int
	{
		int i = int(floorf(t));
		if (clamp)
			return i < 0 ? 0 : (i >= size ? size - 1 : i);
		if (flip)
			return ((i & size) ? ~i : i) & (size - 1);
		return i & (size - 1);
	};

	for (int y = miny; y <= maxy; y++)
	{
		if (nparts > 1 && ((y >> 4) % nparts) != part)
			continue;
		const float py = y + 0.5f;
		u32 *dst = &bitmap.pix(y, 0);
		float *zrow = &m_depth[y * WIDTH];
		for (int x = minx; x <= maxx; x++)
		{
			const float px = x + 0.5f;
			const float e0 = ((v1.x - px) * (v2.y - py) - (v2.x - px) * (v1.y - py)) * inv_area;
			const float e1 = ((v2.x - px) * (v0.y - py) - (v0.x - px) * (v2.y - py)) * inv_area;
			const float e2 = 1.0f - e0 - e1;
			if (e0 < 0 || e1 < 0 || e2 < 0)
				continue;

			const float z = e0 * v0.z + e1 * v1.z + e2 * v2.z;
			const float zold = zrow[x];
			bool pass;
			switch (depthmode)
			{
			case 0: pass = false; break;
			case 1: pass = z < zold; break;
			case 2: pass = z == zold; break;
			case 3: pass = z <= zold; break;
			case 4: pass = z > zold; break;
			case 5: pass = z != zold; break;
			case 6: pass = z >= zold; break;
			default: pass = true; break;
			}
			if (!pass)
				continue;

			// base colour
			int ca, cr, cg, cb;
			if (gouraud)
			{
				ca = int(e0 * (v0.col >> 24) + e1 * (v1.col >> 24) + e2 * (v2.col >> 24));
				cr = int(e0 * ((v0.col >> 16) & 255) + e1 * ((v1.col >> 16) & 255) + e2 * ((v2.col >> 16) & 255));
				cg = int(e0 * ((v0.col >> 8) & 255) + e1 * ((v1.col >> 8) & 255) + e2 * ((v2.col >> 8) & 255));
				cb = int(e0 * (v0.col & 255) + e1 * (v1.col & 255) + e2 * (v2.col & 255));
			}
			else
			{
				ca = v2.col >> 24; cr = (v2.col >> 16) & 255; cg = (v2.col >> 8) & 255; cb = v2.col & 255;
			}

			int r = cr, g = cg, b = cb, a = ca;
			if (tex)
			{
				const float w0 = e0 * v0.z, w1 = e1 * v1.z, w2 = e2 * v2.z;
				const float wsum = w0 + w1 + w2;
				const float inv = 1.0f / wsum;
				const float un = (w0 * v0.u + w1 * v1.u + w2 * v2.u) * inv;
				const float vn = (w0 * v0.v + w1 * v1.v + w2 * v2.v) * inv;

				// level of detail from the analytic screen space derivatives of u and v
				int level = 0;
				if (nlev > 1)
				{
					const float dudx = (dUdx - un * dWdx) * inv, dvdx = (dVdx - vn * dWdx) * inv;
					const float dudy = (dUdy - un * dWdy) * inv, dvdy = (dVdy - vn * dWdy) * inv;
					const float bw = float(tex->lv[0].w), bh = float(tex->lv[0].h);
					const float rx = dudx * dudx * bw * bw + dvdx * dvdx * bh * bh;
					const float ry = dudy * dudy * bw * bw + dvdy * dvdy * bh * bh;
					const float rho2 = std::max(rx, ry);
					if (rho2 > 1.0f)
					{
						// level = round(0.5 * log2(rho2)): from the float exponent, no log2f per pixel
						u32 bits;
						memcpy(&bits, &rho2, 4);
						const int e = int((bits >> 23) & 255) - 127;
						level = std::min(nlev - 1, ((e + 1) >> 1));
					}
				}
				const tex_level &lv = tex->lv[level];

				u32 t;
				if (bilinear)
				{
					const float fu = un * lv.w - 0.5f, fv = vn * lv.h - 0.5f;
					const int x0 = int(floorf(fu)), y0 = int(floorf(fv));
					const float ax = fu - x0, ay = fv - y0;
					const int xa = addr_uv(float(x0), lv.w, flipuv & 2, clampuv & 2), xb = addr_uv(float(x0 + 1), lv.w, flipuv & 2, clampuv & 2);
					const int ya = addr_uv(float(y0), lv.h, flipuv & 1, clampuv & 1), yb = addr_uv(float(y0 + 1), lv.h, flipuv & 1, clampuv & 1);
					const u32 t00 = lv.pix[ya * lv.w + xa], t10 = lv.pix[ya * lv.w + xb], t01 = lv.pix[yb * lv.w + xa], t11 = lv.pix[yb * lv.w + xb];
					const int w00 = int((1 - ax) * (1 - ay) * 256), w10 = int(ax * (1 - ay) * 256), w01 = int((1 - ax) * ay * 256), w11 = 256 - w00 - w10 - w01;
					t = 0;
					for (int sh = 0; sh < 32; sh += 8)
						t |= (((t00 >> sh) & 255) * w00 + ((t10 >> sh) & 255) * w10 + ((t01 >> sh) & 255) * w01 + ((t11 >> sh) & 255) * w11) >> 8 << sh;
				}
				else
				{
					const int tx = addr_uv(un * lv.w, lv.w, flipuv & 2, clampuv & 2);
					const int ty = addr_uv(vn * lv.h, lv.h, flipuv & 1, clampuv & 1);
					t = lv.pix[ty * lv.w + tx];
				}
				const int ta = ignore_texa ? 255 : (t >> 24);
				const int tr = (t >> 16) & 255, tg = (t >> 8) & 255, tb = t & 255;
				switch (tsinst)
				{
				case 0: r = tr; g = tg; b = tb; a = ta; break;
				case 1: r = tr * cr / 255; g = tg * cg / 255; b = tb * cb / 255; a = ta; break;
				case 2: r = (tr * ta + cr * (255 - ta)) / 255; g = (tg * ta + cg * (255 - ta)) / 255; b = (tb * ta + cb * (255 - ta)) / 255; a = ca; break;
				default: r = tr * cr / 255; g = tg * cg / 255; b = tb * cb / 255; a = ta * ca / 255; break;
				}
			}
			// (the TSP "use alpha" bit is clear on this board's translucent polygons while they still blend on
			// source alpha, so alpha is always taken from the texture/vertex)
			(void)use_alpha;

			if (!opaque_blend)
			{
				const u32 d = dst[x];
				const int dr = (d >> 16) & 255, dg = (d >> 8) & 255, db = d & 255;
				auto factor = [&](int inst, bool src, int sr, int sg, int sb, int sa, int c) -> int
				{
					// returns factor*255 for colour channel c (0=r,1=g,2=b)
					const int other[3] = { dr, dg, db };
					const int own[3] = { sr, sg, sb };
					switch (inst)
					{
					case 0: return 0;
					case 1: return 255;
					case 2: return src ? other[c] : own[c];
					case 3: return 255 - (src ? other[c] : own[c]);
					case 4: return sa;
					case 5: return 255 - sa;
					case 6: return 255;
					default: return 0;
					}
				};
				const int s[3] = { r, g, b }, dd[3] = { dr, dg, db };
				int o[3];
				for (int c = 0; c < 3; c++)
					o[c] = std::min(255, (s[c] * factor(srcinst, true, r, g, b, a, c) + dd[c] * factor(dstinst, false, r, g, b, a, c)) / 255);
				r = o[0]; g = o[1]; b = o[2];
			}
			else
			{
				// opaque: source alpha 1, dest 0
			}
			dst[x] = 0xff000000 | (std::clamp(r, 0, 255) << 16) | (std::clamp(g, 0, 255) << 8) | std::clamp(b, 0, 255);
			if (zwrite)
				zrow[x] = z;
		}
	}
}

void neon250_renderer::process_list(u32 pagetable, u32 words, bitmap_rgb32 &bitmap)
{
	if (!pagetable || !words)
		return;
	const u32 pages = (words + 1023) / 1024;
	u32 state[5] = { 0, 0, 0, 0, 0 };
	bool have_state = false;
	const tex_entry *tex = nullptr;

	auto list_word = [&](u32 index) -> u32
	{
		const u32 page = rd(pagetable + (index >> 10) * 4);
		return rd((page << 12) + (index & 1023) * 4);
	};

	for (u32 i = 0; i < words && (i >> 10) < pages;)
	{
		const u32 ent = list_word(i);
		if (ent == 0 || ent == 0xdeadbeef)
			break;
		i++;
		if (ent & 0x80000000)
		{
			for (int k = 0; k < 5; k++)
				state[k] = list_word(i++);
			have_state = true;
			tex = (state[2] & 0x02000000) ? get_texture(state[4], state[3]) : nullptr;
		}
		if (!have_state)
			continue;

		const u32 mask = (ent >> 25) & 0x3f;
		const u32 skip = (ent >> 21) & 7;
		const u32 off = ent & 0x1fffff;
		const u32 vsize = (skip + 3) * 4;
		const bool textured = (state[2] & 0x02000000);
		auto fetch = [&](u32 index, vtx &v)
		{
			const u32 a = state[0] + off * 4 + vsize * index;
			v.x = rdf(a); v.y = rdf(a + 4); v.z = rdf(a + 8);
			if (textured) { v.u = rdf(a + 12); v.v = rdf(a + 16); v.col = rd(a + 20); }
			else { v.u = v.v = 0; v.col = rd(a + 12); }
		};
		for (int k = 0; k < 6; k++)
		{
			if (!(mask & (0x20 >> k)))
				continue;
			vtx a, b, c;
			fetch(k, a); fetch(k + 1, b); fetch(k + 2, c);
			draw_triangle(a, b, c, state[2], state[3], textured ? tex : nullptr, bitmap);
		}
	}
	m_lists++;
}

void neon250_renderer::rasterize(bitmap_rgb32 &bitmap)
{
	// Rows are dealt to the workers in groups of 16, so every pixel is still drawn by one thread in list order.
	static const int nthreads = []
	{
		const unsigned hc = std::thread::hardware_concurrency();
		return int(std::clamp(hc > 2 ? hc - 2 : 1u, 1u, 8u));
	}();

	auto work = [this, &bitmap](int part, int nparts)
	{
		for (const tri_job &t : m_queue)
			draw_triangle_raw(t.v0, t.v1, t.v2, t.isp, t.tsp, t.tex, bitmap, part, nparts);
	};
	if (nthreads <= 1 || m_queue.size() < 32)
	{
		work(0, 1);
		return;
	}
	std::vector<std::thread> workers;
	workers.reserve(nthreads - 1);
	for (int i = 1; i < nthreads; i++)
		workers.emplace_back(work, i, nthreads);
	work(0, nthreads);
	for (auto &w : workers)
		w.join();
}

void neon250_renderer::render(u32 ctx, bitmap_rgb32 &bitmap, u32 clear_color)
{
	m_frame++;
	m_tris = 0;
	m_lists = 0;
	bitmap.fill(clear_color);
	std::fill(m_depth.begin(), m_depth.end(), 0.0f);

	m_queue.clear();
	// three list descriptors: flags, ?, size in words, page table
	for (int l = 0; l < 3; l++)
	{
		const u32 d = ctx + 0x18 + 0x10 * l;
		process_list(rd(d + 12), rd(d + 8), bitmap);
	}
	m_tris = int(m_queue.size());
	rasterize(bitmap);



	// drop textures not used for a while
	if ((m_frame & 255) == 0)
		for (auto it = m_texcache.begin(); it != m_texcache.end();)
			it = (m_frame - it->second.frame > 512) ? m_texcache.erase(it) : std::next(it);
}



class atvtrack_state : public driver_device
{
public:
	atvtrack_state(const machine_config &mconfig, device_type type, const char *tag)
		: driver_device(mconfig, type, tag),
		m_maincpu(*this, "maincpu"),
		m_subcpu(*this, "subcpu"),
		m_gpuram(*this, "gpuram"),
		m_adc(*this, "adc"),
		m_dac(*this, "dac"),
		m_jp8(*this, "JP8"),
		m_jp3(*this, "JP3"),
		m_jp10(*this, "JP10"),
		m_analog(*this, "ANALOG%u", 0U) { }

	void atvtrack(machine_config &config);

protected:
	u64 control_r(offs_t offset, u64 mem_mask = ~0);
	void control_w(offs_t offset, u64 data, u64 mem_mask = ~0);
	u64 nand_data_r();
	void nand_data_w(u64 data);
	void nand_cmd_w(u64 data);
	void nand_addr_w(u64 data);
	u64 ioport_r(offs_t offset);
	void ioport_w(offs_t offset, u64 data);
	u32 gpu_r(offs_t offset);
	void gpu_w(offs_t offset, u32 data);
	virtual void machine_start() override ATTR_COLD;
	virtual void machine_reset() override ATTR_COLD;
	virtual void video_start() override ATTR_COLD;
	u32 screen_update_atvtrack(screen_device &screen, bitmap_rgb32 &bitmap, const rectangle &cliprect);
	inline u32 decode64_32(offs_t offset64, u64 data, u64 mem_mask, offs_t &offset32);
	[[maybe_unused]] void logbinary(u32 data, int high, int low);

	memory_region *m_nandregion = nullptr;
	int m_nandcommand[4]{}, m_nandoffset[4]{}, m_nandaddressstep = 0, m_nandaddress[4]{};
	u32 m_area1_data[4]{};

	required_device<sh7750s_device> m_maincpu;
	required_device<sh7750s_device> m_subcpu;
	required_shared_ptr<u64> m_gpuram;
	required_device<adc0838_device> m_adc;
	required_device<gaelco_atvtrack_dac_device> m_dac;
	void dac_irq(int state) { m_maincpu->set_input_line(SH4_IRL1, state ? ASSERT_LINE : CLEAR_LINE); } // ICR selects 4 independent IRL lines
	required_ioport m_jp8;
	required_ioport m_jp3;
	required_ioport m_jp10;
	required_ioport_array<4> m_analog;
	int m_adc_edges = 0, m_adc_clk = 0;
	u16 m_pa = 0xffff; // last value written to the 16 bit GPIO port A (bits 15-11 select a bank)
	double adc_callback(u8 input);

	neon250_renderer m_renderer;
	bitmap_rgb32 m_screen_bitmap;
	void gpu_render_kick();

	u16 gpu_irq_pending = 0;
	u16 gpu_irq_mask = 0;
	void gpu_irq_test();
	void gpu_irq_set(int);

	void atvtrack_main_map(address_map &map) ATTR_COLD;
	void atvtrack_main_port(address_map &map) ATTR_COLD;
	void atvtrack_sub_map(address_map &map) ATTR_COLD;
	void atvtrack_sub_port(address_map &map) ATTR_COLD;

	bool m_slaverun = false;
};


class smashdrv_state : public atvtrack_state
{
public:
	smashdrv_state(const machine_config &mconfig, device_type type, const char *tag)
		: atvtrack_state(mconfig, type, tag) { }

	void smashdrv(machine_config &config);

private:
	virtual void machine_start() override ATTR_COLD;
	virtual void machine_reset() override ATTR_COLD;

	void smashdrv_main_map(address_map &map) ATTR_COLD;
	void smashdrv_main_port(address_map &map) ATTR_COLD;
	void smashdrv_sub_map(address_map &map) ATTR_COLD;

	// PRG flash (the 4MB chip at IC23): read as plain memory, programmed and erased with the usual AMD command
	// sequences on byte addresses (0x555 / 0xaaa unlock). It holds the game's settings.
	u64 prgflash_r(offs_t offset);
	void prgflash_w(offs_t offset, u64 data, u64 mem_mask);
	void prgflash_command(u32 addr, u8 data);
	std::unique_ptr<u8[]> m_prgflash;
	int m_flashstate = 0;

	// The game keeps its master volume (0.0 - 1.0 float, shown as "Vol 00"-"Vol 99") in main RAM and starts
	// with 0 on a machine that has never had its volume buttons pressed, so it would boot silent.
	// Seed a usable default shortly after boot; the volume buttons then work as normal.
	TIMER_CALLBACK_MEMBER(default_volume);
	emu_timer *m_volume_timer = nullptr;
};

void atvtrack_state::logbinary(u32 data, int high=31, int low=0)
{
	u32 s;
	int z;

	s=1 << high;
	for (z = high;z >= low;z--) {
		if (data & s)
			logerror("1");
		else
			logerror("0");
		s=s >> 1;
	}
}

inline u32 atvtrack_state::decode64_32(offs_t offset64, u64 data, u64 mem_mask, offs_t &offset32)
{
	if (ACCESSING_BITS_0_31) {
		offset32 = offset64 << 1;
		return (u32)data;
	}
	if (ACCESSING_BITS_32_63) {
		offset32 = (offset64 << 1)+1;
		return (u32)(data >> 32);
	}
	logerror("Wrong word size in external access\n");
	//machine().debug_break();
	return 0;
}

u64 atvtrack_state::control_r(offs_t offset, u64 mem_mask)
{
	offs_t addr = 0;
	decode64_32(offset, 0, mem_mask, addr);
	if (addr == (0x00020000-0x00020000)/4)
		return -1;
	else if (addr == (0x00020004-0x00020000)/4)
		return -1;
	return -1;
}

void atvtrack_state::control_w(offs_t offset, u64 data, u64 mem_mask)
{
	offs_t addr = 0;
	u32 dat = decode64_32(offset, data, mem_mask, addr);
	//u32 old = m_area1_data[addr];
	m_area1_data[addr] = dat;
	if (addr == (0x00020000-0x00020000)/4) {
		if ((data & 4) && m_slaverun)
			m_subcpu->set_input_line(INPUT_LINE_RESET, CLEAR_LINE);
		else
			m_subcpu->set_input_line(INPUT_LINE_RESET, ASSERT_LINE);
	}
//  logerror("Write %08x at %08x ",dat, 0x20000+addr*4+0);
//  logbinary(dat);
//  logerror("\n");
}

u64 atvtrack_state::nand_data_r()
{
	u32 dat = 0;
	for (int c = 3; c >= 0; c--) {
		if (m_nandcommand[c] <= 0x50) {
			u32 addr = m_nandaddress[c] + m_nandoffset[c];
			dat = (dat << 8) | m_nandregion->as_u8(addr + c);
			m_nandoffset[c] += 4;
		}
		else
			dat = (dat << 8) | 0xc0;
	}
	return dat;
}

void atvtrack_state::nand_data_w(u64 data)
{
	for (int c = 0; c < 4; c++) {
		if (m_nandcommand[c] == 0x80) {
			u8 *ptr = m_nandregion->base();
			u32 addr = m_nandaddress[c] + m_nandoffset[c] + c;
			ptr[addr] &= data & 0xff;
			m_nandoffset[c] += 4;
		}
		data >>= 8;
	}
}

void atvtrack_state::nand_cmd_w(u64 data)
{
	m_nandaddressstep = 0;
	for (int c = 0;c < 4;c++) {
		m_nandcommand[c] = data & 0xff;
		if (m_nandcommand[c] == 0x00) {
			m_nandoffset[c] = 0;
		} else if (m_nandcommand[c] == 0x01) {
			m_nandoffset[c] = 256*4;
		} else if (m_nandcommand[c] == 0x50) {
			m_nandoffset[c] = 512*4;
		} else if (m_nandcommand[c] == 0x90) {
		} else if (m_nandcommand[c] == 0xff) {
		} else if (m_nandcommand[c] == 0x80) {
		} else if (m_nandcommand[c] == 0x60) {
			m_nandaddressstep = 1;
			m_nandaddress[c] = 0;
		} else if (m_nandcommand[c] == 0x70) {
		} else if (m_nandcommand[c] == 0x10) {
		} else if (m_nandcommand[c] == 0xd0) {
			u8 *ptr = m_nandregion->base();
			u32 addr = m_nandaddress[c] + c;
			for (int i = 0; i < 32 * 528; i++)
				ptr[addr + i * 4] = 0xff;
		} else {
			m_nandcommand[c] = 0xff;
		}
		data=data >> 8;
	}
}

void atvtrack_state::nand_addr_w(u64 data)
{
	for (int c = 0;c < 4;c++) {
		if (m_nandaddressstep == 0) {
			m_nandaddress[c] = (data & 0xff)*4;
		} else if (m_nandaddressstep == 1) {
			m_nandaddress[c] = m_nandaddress[c]+(data & 0xff)*0x840;
		} else if (m_nandaddressstep == 2) {
			m_nandaddress[c] = m_nandaddress[c]+(data & 0xff)*0x84000;
		}
		data = data >> 8;
	}
	m_nandaddressstep++;
}

void atvtrack_state::gpu_irq_test()
{
	if (gpu_irq_pending & ~gpu_irq_mask)
		m_subcpu->sh4_set_irln_input(14);
	else
		m_subcpu->sh4_set_irln_input(15);
}

void atvtrack_state::gpu_irq_set(int bit)
{
	gpu_irq_pending |= 1 << bit;
	gpu_irq_test();
}

u32 atvtrack_state::gpu_r(offs_t offset)
{
	switch (offset)
	{
	case 0x70/4:
		return gpu_irq_pending;
	case 0x74/4:
		return gpu_irq_mask;
	default:
		logerror("GPU: unhandled reg read @ %04X\n", offset * 4);
		return 0;
	}
}

void atvtrack_state::gpu_w(offs_t offset, u32 data)
{
	switch (offset)
	{
	case 0x00/4:
		// not really required, game code will go even if GPU CPU shows no signs of live
		if (data)   // internal CPU start ?
			m_subcpu->space(AS_PROGRAM).write_byte(0x18001350, 1); // simulate GPUs internal CPU reply to skip busy loop
		break;
	case 0x70/4:
		gpu_irq_pending &= ~data;
		gpu_irq_test();
		break;
	case 0x74/4:
		gpu_irq_mask = data;
		gpu_irq_test();
		break;
	case 0xb0/4:
		if (data == 0x0001)
		{
			gpu_render_kick();
			gpu_irq_set(7);
		}
		// not really required, game code will go even if GPU CPU shows no signs of live
		if (data == 0x8001)
			m_subcpu->space(AS_PROGRAM).write_byte(0x18814804, 1); // simulate GPUs internal CPU reply to skip busy loop
		break;
	default:
		logerror("GPU: unhandled reg write @ %04X data %08X\n", offset * 4, data);
		break;
	}
}

u64 atvtrack_state::ioport_r(offs_t offset)
{
	if (offset == SH4_IOPORT_16/8) {
		// switches and the ADC data out are read through two banks selected by port A bits 15-11, active low
		u16 v = 0xffff;
		switch (m_pa & 0xf800)
		{
		case 0xf000: // JP10: bit 9 ADC data out, 8 coin, 6 test, 5 down, 4 service, 3 up
			v = (m_jp10->read() & 0x01f8) | 0xfe07;
			if (!m_adc->do_read())
				v &= ~0x0200;
			else
				v |= 0x0200;
			break;
		case 0xe800: // JP8: bits 10-3 game specific switches
			v = ((m_jp8->read() & 0xff) << 3) | 0xf807;
			break;
		case 0xf800: // JP3 (port bits as inputs): bits 10-3, game specific
			v = ((m_jp3->read() & 0xff) << 3) | 0xf807;
			break;
		default:
			break;
		}
		return 0xffffffffffff0000ULL | v;
	}
	return 0;
}

double atvtrack_state::adc_callback(u8 input)
{
	switch (input)
	{
	case ADC083X_CH0:
	case ADC083X_CH1:
	case ADC083X_CH2:
	case ADC083X_CH3:
		return 5.0 * m_analog[input]->read() / 255.0;
	case ADC083X_VREF:
		return 5.0;
	default:
		return 0.0;
	}
}

void atvtrack_state::ioport_w(offs_t offset, u64 data)
{
	// SH4 GPIO port A used in this way:
	// bits 15-11  O - port select: F002 (In)  E802 (In)  F800 (Out)        7800 (Out)
	//                              JP10 conn  JP8 conn   ADC/DAC control   System Control and/or diagnostics
	// bit  10    IO - data bit                 *1        GPO JP3           motion enabled indicator ?
	// bit  9     IO - data bit     ADC data    *1        GPO JP3           sound (AMP) enable ? (ATV - set/res then music starts/ends, SD - always set)
	// bit  8     IO - data bit     Coin        *1        GPO JP3            \ slave CPU start/stop/reset related
	// bit  7     IO - data bit                 *1        always 1           / set to 30 during slave CPU test, then to 10
	// bit  6     IO - data bit     Test                  ADC CS, ~DAC CS    unused
	// bit  5     IO - data bit     Down                  ADC CLK, ~DAC CLK  unused
	// bit  4     IO - data bit     Service               DAC data JP6       lamp
	// bit  3     IO - data bit     Up                    ADC data JP5       coin counter
	// bit  2     I  - unk, (SD: 1 = FPGA ready after config)
	// bit  1      O - data bits operation direction: 0 - output, 1 - input (SD: FPGA config data)
	// bit  0      O - unk (SD: FPGA config clock)
	// ADC and DAC is 4-channel SPI devices
	// Switch inputs is active low, *1 - game specific inputs
	// All above IO multiplexing, ADC and DAC implemented in FPGA

#ifdef SPECIALMODE
	u64 d;
	static int cnt=0;
	sh4_device_dma dm;
#endif

	if (offset == SH4_IOPORT_16/8) {
		m_pa = data & 0xffff;
		if ((m_pa & 0xf800) == 0xf800)
		{
			// ADC0838 on bit-banged lines: CS bit 6 (active low), CLK bit 5, DI bit 3
			const int cs = BIT(m_pa, 6), di = BIT(m_pa, 3), clk = BIT(m_pa, 5);
			m_adc->cs_write(cs);
			if (cs)
				m_adc_edges = 0;
			m_adc->di_write(di);
			m_adc->clk_write(clk);
			if (!cs)
			{
				if (!m_adc_clk && clk)
					m_adc_edges++;
				else if (m_adc_clk && !clk && m_adc_edges == 5)
				{
					// the game reads the first data bit right after the 5 command clocks, so the chip's null bit has
					// to be clocked out already
					m_adc->clk_write(1);
					m_adc->clk_write(0);
					m_adc_edges++;
				}
			}
			m_adc_clk = clk;
		}
		if ((data & 0xf000) == 0x7000) {
			if (data & 0x0100)
				m_slaverun = true;
		}
//      logerror("SH4 16bit i/o port write ");
//      logbinary((u32)data, 15, 0);
//      logerror("\n");
	}
#ifdef SPECIALMODE
	if (offset == SH4_IOPORT_DMA/8) {
		dm.buffer = &d;
		dm.channel = data & 0xffff;
		dm.length = 1;
		dm.size = 4;
		if (cnt == 0)
			d=0x12340153;
		else
			d=0x11223344;
		if (cnt == 0)
			sh4_dma_data(cpu,&dm);
		else
			sh4_dma_data(cpu,&dm);
		cnt++;
	}
#endif
}

void atvtrack_state::video_start()
{
	m_screen_bitmap.allocate(neon250_renderer::WIDTH, neon250_renderer::HEIGHT);
	m_screen_bitmap.fill(0xff000000);
	m_renderer.set_ram(reinterpret_cast<const u32 *>(&m_gpuram[0]));
}

u32 atvtrack_state::screen_update_atvtrack(screen_device &screen, bitmap_rgb32 &bitmap, const rectangle &cliprect)
{
	copybitmap(bitmap, m_screen_bitmap, 0, 0, 0, 0, cliprect);
	return 0;
}

// A render job is a 0x40 byte record in a ring in GPU RAM; the sub CPU advances a write pointer (in
// bytes, 0x40 per job) at 0x18811020 and pokes the kick register. The record's fourth word points to
// the frame's context block.
void atvtrack_state::gpu_render_kick()
{
	const u32 wp = m_renderer.rd(0x18811020);
	const u32 job = 0x18811f80 + (wp & 0x3fff) - 0x40;
	if (m_renderer.rd(job) != 0x40)
	{
		logerror("GPU: render kick without a valid job at %08x (wp %08x)\n", job, wp);
		return;
	}
	const u32 ctx = m_renderer.rd(job + 12);
	if ((ctx & 0xfe000000) != neon250_renderer::RAM_BASE)
	{
		logerror("GPU: render job %08x has a bad context %08x\n", job, ctx);
		return;
	}
	m_renderer.render(ctx, m_screen_bitmap, 0xff000000);
}

void get_altera10ke_eab(u8* dst, u8 *pof, int eab)
{
	// extract Altera FLEX 10KE 4kbit Embedded Array Block (EAB)
	u32 startbit = 0x45b * 8 + 1 + (0x25e6 * 8) * eab;  // base ofsset actually depends on POF header size, however this EPC1PC8 dump havent it (bad dumper software?)

	for (u32 bit = 0; bit < 4096; bit++)
	{
		u32 tbit = bitswap<16>(bit, 15, 14, 13, 12,
			9, 8, 7, 6, 5, 4, 3,
			11, 10,
			2, 1, 0);
		tbit ^= 0x0fc0;
		u32 pofbit = startbit + (tbit & 0x1f) + (((tbit >> 5) & 1) * 63) + (((tbit >> 6) & 0x3f) * 0x4D * 8);
		dst[bit / 8] &= ~(1 << (bit & 7));
		dst[bit / 8] |= ((pof[pofbit / 8] >> (pofbit & 7)) & 1) << (bit & 7);
	}
}

void atvtrack_state::machine_start()
{
	m_nandaddressstep = 0;
	m_nandregion = memregion("nand");
}

void atvtrack_state::machine_reset()
{
	std::vector<u8> tdata(1024);
	u8 *pof = memregion("fpga")->base();

	// first 2 Altera's EABs is 2xSH4s shared RAM, its initial content is boot loader
	// extract EABs 0 and 1 from POF
	get_altera10ke_eab(&tdata[0], pof, 0);
	get_altera10ke_eab(&tdata[512], pof, 1);

	// deshuffle data bits and put it to shared RAM
	u8 *dst = (u8*)(m_maincpu->space(AS_PROGRAM).get_write_ptr(0));
	for (u32 i = 0; i < 256; i++)
	{
		u16 lword = tdata[i * 2 + 512] | (tdata[i * 2 + 513] << 8);
		u16 hword = tdata[i * 2] | (tdata[i * 2 + 1] << 8);
		lword = bitswap<16>(lword, 7, 9, 0, 10, 3, 11, 4, 12, 2, 15, 1, 13, 6, 8, 5, 14);
		hword = bitswap<16>(hword, 5, 10, 7, 9, 6, 13, 3, 15, 2, 11, 1, 8, 0, 12, 4, 14);
		dst[i * 4 + 0] = lword & 0xff;
		dst[i * 4 + 1] = lword >> 8;
		dst[i * 4 + 2] = hword & 0xff;
		dst[i * 4 + 3] = hword >> 8;
	}

	m_subcpu->set_input_line(INPUT_LINE_RESET, ASSERT_LINE);
	gpu_irq_pending = 0;
	gpu_irq_mask = 0xFFFF;
}



u64 smashdrv_state::prgflash_r(offs_t offset)
{
	u64 v;
	memcpy(&v, &m_prgflash[(offset << 3) & 0x3ffff8], 8);
	return v;
}

void smashdrv_state::prgflash_w(offs_t offset, u64 data, u64 mem_mask)
{
	for (int lane = 0; lane < 8; lane++)
		if ((mem_mask >> (lane * 8)) & 0xff)
			prgflash_command(((offset << 3) + lane) & 0x3fffff, u8(data >> (lane * 8)));
}

void smashdrv_state::prgflash_command(u32 addr, u8 data)
{
	const u32 cmdaddr = addr & 0xfff;
	switch (m_flashstate)
	{
	case 0:
		m_flashstate = (cmdaddr == 0x555 && data == 0xaa) ? 1 : 0;
		break;
	case 1:
		m_flashstate = (cmdaddr == 0xaaa && data == 0x55) ? 2 : 0;
		break;
	case 2:
		if (cmdaddr == 0x555 && data == 0xa0)
			m_flashstate = 3; // program one byte
		else if (cmdaddr == 0x555 && data == 0x80)
			m_flashstate = 4; // erase
		else
			m_flashstate = 0;
		break;
	case 3:
		m_prgflash[addr] &= data;
		m_flashstate = 0;
		break;
	case 4:
		m_flashstate = (cmdaddr == 0x555 && data == 0xaa) ? 5 : 0;
		break;
	case 5:
		m_flashstate = (cmdaddr == 0xaaa && data == 0x55) ? 6 : 0;
		break;
	case 6:
		if (data == 0x30)
			std::fill_n(&m_prgflash[addr & ~0xffffU], 0x10000, 0xff); // sector erase (64KB)
		else if (data == 0x10)
			std::fill_n(&m_prgflash[0], 0x400000, 0xff);               // chip erase
		m_flashstate = 0;
		break;
	}
}

void smashdrv_state::machine_start()
{
	m_prgflash = std::make_unique<u8[]>(0x400000);
	memcpy(m_prgflash.get(), memregion("data")->base(), 0x400000);
	save_pointer(NAME(m_prgflash), 0x400000);
	save_item(NAME(m_flashstate));
	m_volume_timer = timer_alloc(FUNC(smashdrv_state::default_volume), this);
}

TIMER_CALLBACK_MEMBER(smashdrv_state::default_volume)
{
	constexpr offs_t VOLUME_ADDR = 0x0c0733fc; // game's master volume float
	address_space &space = m_maincpu->space(AS_PROGRAM);
	if (space.read_dword(VOLUME_ADDR) == 0)
	{
		const float vol = 0.7f;
		u32 bits;
		memcpy(&bits, &vol, sizeof(bits));
		space.write_dword(VOLUME_ADDR, bits);
	}
}

void smashdrv_state::machine_reset()
{
	m_volume_timer->adjust(attotime::from_seconds(10));
	m_slaverun = false;
	m_subcpu->set_input_line(INPUT_LINE_RESET, ASSERT_LINE);
	gpu_irq_pending = 0;
	gpu_irq_mask = 0xFFFF;
}

// ATV Track

void atvtrack_state::atvtrack_main_map(address_map &map)
{
	map(0x00000000, 0x000003ff).ram().share("sharedmem");
	map(0x00020000, 0x00020007).rw(FUNC(atvtrack_state::control_r), FUNC(atvtrack_state::control_w)); // control registers
	map(0x00020040, 0x0002007f).rw(m_dac, FUNC(gaelco_atvtrack_dac_device::buffer_r), FUNC(gaelco_atvtrack_dac_device::buffer_w)); // audio DAC buffer
	map(0x14000000, 0x14000007).rw(FUNC(atvtrack_state::nand_data_r), FUNC(atvtrack_state::nand_data_w));
	map(0x14100000, 0x14100007).w(FUNC(atvtrack_state::nand_cmd_w));
	map(0x14200000, 0x14200007).w(FUNC(atvtrack_state::nand_addr_w));
	map(0x0c000000, 0x0c7fffff).ram();
}

void atvtrack_state::atvtrack_main_port(address_map &map)
{
	map(0x00, 0x1f).rw(FUNC(atvtrack_state::ioport_r), FUNC(atvtrack_state::ioport_w));
}

// Smashing Drive

void smashdrv_state::smashdrv_main_map(address_map &map)
{
	map(0x00000000, 0x03ffffff).rom();
	map(0x0c000000, 0x0c7fffff).ram();
	map(0x10000000, 0x100003ff).ram().share("sharedmem");
	map(0x10000400, 0x10000407).rw(FUNC(smashdrv_state::control_r), FUNC(smashdrv_state::control_w)); // control registers
	map(0x10000440, 0x1000047f).rw(m_dac, FUNC(gaelco_atvtrack_dac_device::buffer_r), FUNC(gaelco_atvtrack_dac_device::buffer_w)); // audio DAC buffer

// 0x10000400 - 0x1000043F control registers
// 0x10000440 - 0x1000047F Audio DAC buffer
	map(0x14000000, 0x143fffff).rw(FUNC(smashdrv_state::prgflash_r), FUNC(smashdrv_state::prgflash_w));
}

void smashdrv_state::smashdrv_main_port(address_map &map)
{
	map(0x00, 0x1f).rw(FUNC(smashdrv_state::ioport_r), FUNC(smashdrv_state::ioport_w));
}

// Sub CPU

// The sub CPU's libc reads its data files straight out of the flash (0x01820000) and the PRG ROM
// (0x143B0000), so both ROMs are visible to it as well. Only the GPU registers overlay the first
// 16K of the PRG window.
void smashdrv_state::smashdrv_sub_map(address_map &map)
{
	map(0x00000400, 0x03ffffff).rom().region("maincpu", 0x400);
	map(0x14000000, 0x143fffff).r(FUNC(smashdrv_state::prgflash_r));
	atvtrack_sub_map(map);
}

void atvtrack_state::atvtrack_sub_map(address_map &map)
{
	map(0x00000000, 0x000003ff).ram().share("sharedmem");
	map(0x0c000000, 0x0cffffff).ram();
	map(0x14000000, 0x14003fff).rw(FUNC(atvtrack_state::gpu_r), FUNC(atvtrack_state::gpu_w));
// 0x14004xxx GPU PCI CONFIG registers
	map(0x18000000, 0x19ffffff).ram().share("gpuram");
// 0x18000000 - 0x19FFFFFF GPU RAM (32MB)
}

void atvtrack_state::atvtrack_sub_port(address_map &map)
{
}


// Both games read the same two switch banks (port A select 0xf002 = JP10, 0xe802 = JP8) and an ADC0838. JP10 is
// the same everywhere; JP8 is game specific (the games' input test screens name the bits they use).
#define ATVTRACK_COMMON_PORTS \
	PORT_START("JP10") /* bank 0xf002, active low */ \
	PORT_BIT( 0x0008, IP_ACTIVE_LOW, IPT_OTHER )    PORT_NAME("Volume Up") PORT_CODE(KEYCODE_9) \
	PORT_BIT( 0x0010, IP_ACTIVE_LOW, IPT_SERVICE1 ) PORT_NAME("Service") \
	PORT_BIT( 0x0020, IP_ACTIVE_LOW, IPT_OTHER )    PORT_NAME("Volume Down") PORT_CODE(KEYCODE_8) \
	PORT_BIT( 0x0040, IP_ACTIVE_LOW, IPT_SERVICE )  PORT_NAME("Test") \
	PORT_BIT( 0x0080, IP_ACTIVE_LOW, IPT_UNKNOWN ) PORT_NAME("JP10 bit 7") \
	PORT_BIT( 0x0100, IP_ACTIVE_LOW, IPT_COIN1 ) \
	PORT_START("ANALOG0") /* ADC channel 0: steering wheel / handlebars */ \
	PORT_BIT( 0xff, 0x80, IPT_PADDLE ) PORT_MINMAX(0x00, 0xff) PORT_SENSITIVITY(100) PORT_KEYDELTA(20) PORT_NAME("Steering") \
	PORT_START("ANALOG1") /* ADC channel 1 */ \
	PORT_BIT( 0xff, 0x00, IPT_PEDAL ) PORT_MINMAX(0x00, 0xff) PORT_SENSITIVITY(100) PORT_KEYDELTA(20) PORT_NAME("ADC Channel 1 (Throttle?)") \
	PORT_START("ANALOG2") /* ADC channel 2 */ \
	PORT_BIT( 0xff, 0x00, IPT_PEDAL2 ) PORT_MINMAX(0x00, 0xff) PORT_SENSITIVITY(100) PORT_KEYDELTA(20) PORT_NAME("ADC Channel 2 (Brake?)") \
	PORT_START("ANALOG3") /* ADC channel 3 */ \
	PORT_BIT( 0xff, 0x00, IPT_AD_STICK_Z ) PORT_MINMAX(0x00, 0xff) PORT_SENSITIVITY(100) PORT_KEYDELTA(20) PORT_NAME("ADC Channel 3")

static INPUT_PORTS_START( smashdrv )
	ATVTRACK_COMMON_PORTS
	PORT_START("JP3") // bank 0xf802 read: unused here, reads as all ones
	PORT_BIT( 0xff, 0xff, IPT_UNUSED )
	PORT_START("JP8") // bank 0xe802, bits 10-3 shifted down by 3, active low. The input test screen names bits 7-9
	PORT_BIT( 0x01, IP_ACTIVE_LOW, IPT_UNKNOWN )  PORT_NAME("JP8 bit 3")
	PORT_BIT( 0x02, IP_ACTIVE_LOW, IPT_UNKNOWN )  PORT_NAME("JP8 bit 4")
	PORT_BIT( 0x04, IP_ACTIVE_LOW, IPT_UNKNOWN )  PORT_NAME("JP8 bit 5")
	PORT_BIT( 0x08, IP_ACTIVE_LOW, IPT_UNKNOWN )  PORT_NAME("JP8 bit 6")
	PORT_BIT( 0x10, IP_ACTIVE_LOW, IPT_BUTTON1 )  PORT_NAME("Accelerator")
	PORT_BIT( 0x20, IP_ACTIVE_LOW, IPT_BUTTON2 )  PORT_NAME("Brake")
	PORT_BIT( 0x40, IP_ACTIVE_LOW, IPT_START1 )   PORT_NAME("Horn / Start")
	PORT_BIT( 0x80, IP_ACTIVE_LOW, IPT_UNKNOWN )  PORT_NAME("JP8 bit 10")
INPUT_PORTS_END

static INPUT_PORTS_START( atvtrack )
	ATVTRACK_COMMON_PORTS
	PORT_START("JP3") // bank 0xf802 read, bits 10-3: one of them is the emergency stop of the motion platform (clear = ok)
	PORT_BIT( 0x01, IP_ACTIVE_HIGH, IPT_UNKNOWN ) PORT_NAME("JP3 bit 3")
	PORT_BIT( 0x02, IP_ACTIVE_HIGH, IPT_UNKNOWN ) PORT_NAME("JP3 bit 4")
	PORT_BIT( 0x04, IP_ACTIVE_HIGH, IPT_UNKNOWN ) PORT_NAME("JP3 bit 5")
	PORT_BIT( 0x08, IP_ACTIVE_HIGH, IPT_UNKNOWN ) PORT_NAME("JP3 bit 6")
	PORT_BIT( 0x10, IP_ACTIVE_HIGH, IPT_UNKNOWN ) PORT_NAME("JP3 bit 7")
	PORT_BIT( 0x20, IP_ACTIVE_HIGH, IPT_UNKNOWN ) PORT_NAME("JP3 bit 8")
	PORT_BIT( 0x40, IP_ACTIVE_HIGH, IPT_UNKNOWN ) PORT_NAME("JP3 bit 9")
	PORT_BIT( 0x80, IP_ACTIVE_HIGH, IPT_UNKNOWN ) PORT_NAME("JP3 bit 10")
	PORT_START("JP8") // bank 0xe802, bits 10-3 shifted down by 3, active low. Bit 10 starts the game, the rest is not mapped yet
	PORT_BIT( 0x01, IP_ACTIVE_LOW, IPT_BUTTON1 )  PORT_NAME("JP8 bit 3")
	PORT_BIT( 0x02, IP_ACTIVE_LOW, IPT_BUTTON2 )  PORT_NAME("JP8 bit 4")
	PORT_BIT( 0x04, IP_ACTIVE_LOW, IPT_BUTTON3 )  PORT_NAME("JP8 bit 5")
	PORT_BIT( 0x08, IP_ACTIVE_LOW, IPT_BUTTON4 )  PORT_NAME("JP8 bit 6")
	PORT_BIT( 0x10, IP_ACTIVE_LOW, IPT_BUTTON5 )  PORT_NAME("JP8 bit 7")
	PORT_BIT( 0x20, IP_ACTIVE_LOW, IPT_BUTTON6 )  PORT_NAME("JP8 bit 8")
	PORT_BIT( 0x40, IP_ACTIVE_LOW, IPT_BUTTON7 )  PORT_NAME("JP8 bit 9")
	PORT_BIT( 0x80, IP_ACTIVE_LOW, IPT_START1 )   PORT_NAME("Start")
INPUT_PORTS_END

#define ATV_CPU_CLOCK XTAL(33'000'000)*6

void atvtrack_state::atvtrack(machine_config &config)
{
	/* basic machine hardware */
	SH7750S(config, m_maincpu, ATV_CPU_CLOCK);
	m_maincpu->set_md(0, 1);
	m_maincpu->set_md(1, 1);
	m_maincpu->set_md(2, 0);
	m_maincpu->set_md(3, 0);
	m_maincpu->set_md(4, 0);
	m_maincpu->set_md(5, 1);
	m_maincpu->set_md(6, 0);
	m_maincpu->set_md(7, 1);
	m_maincpu->set_md(8, 0);
	m_maincpu->set_sh4_clock(ATV_CPU_CLOCK);
	m_maincpu->set_addrmap(AS_PROGRAM, &atvtrack_state::atvtrack_main_map);
	m_maincpu->set_addrmap(AS_IO, &atvtrack_state::atvtrack_main_port);

	SH7750S(config, m_subcpu, ATV_CPU_CLOCK);
	m_subcpu->set_md(0, 1);
	m_subcpu->set_md(1, 1);
	m_subcpu->set_md(2, 0);
	m_subcpu->set_md(3, 0);
	m_subcpu->set_md(4, 0);
	m_subcpu->set_md(5, 1);
	m_subcpu->set_md(6, 0);
	m_subcpu->set_md(7, 1);
	m_subcpu->set_md(8, 0);
	m_subcpu->set_sh4_clock(ATV_CPU_CLOCK);
	m_subcpu->set_addrmap(AS_PROGRAM, &atvtrack_state::atvtrack_sub_map);
	m_subcpu->set_addrmap(AS_IO, &atvtrack_state::atvtrack_sub_port);

	/* video hardware */
	screen_device &screen(SCREEN(config, "screen"));
	screen.set_refresh_hz(60);
	screen.set_vblank_time(ATTOSECONDS_IN_USEC(2500));  /* not accurate */
	screen.set_size(640, 480);
	screen.set_visarea(0, 640-1, 0, 480-1);
	screen.set_screen_update(FUNC(atvtrack_state::screen_update_atvtrack));

	PALETTE(config, "palette").set_entries(0x1000);

	ADC0838(config, m_adc);
	m_adc->set_input_callback(FUNC(atvtrack_state::adc_callback));

	SPEAKER(config, "speaker", 2).front();
	GAELCO_ATVTRACK_DAC(config, m_dac);
	m_dac->irq_cb().set(FUNC(atvtrack_state::dac_irq));
	m_dac->add_route(0, "speaker", 1.0, 0);
	m_dac->add_route(1, "speaker", 1.0, 1);
}

void smashdrv_state::smashdrv(machine_config &config)
{
	atvtrack(config);
	m_maincpu->set_addrmap(AS_PROGRAM, &smashdrv_state::smashdrv_main_map);
	m_maincpu->set_addrmap(AS_IO, &smashdrv_state::smashdrv_main_port);
	m_subcpu->set_addrmap(AS_PROGRAM, &smashdrv_state::smashdrv_sub_map);
}


ROM_START( atvtrack )
	ROM_REGION( 0x4200000, "nand", ROMREGION_ERASEFF) // NAND roms, contain additional data hence the sizes
	ROM_LOAD32_BYTE("15.bin", 0x0000000, 0x1080000, CRC(84eaede7) SHA1(6e6230165c3bb35e49c660dfd0d07c132ed89e6a) )
	ROM_LOAD32_BYTE("20.bin", 0x0000001, 0x1080000, CRC(649dc331) SHA1(0cac2d0c15dd564c7fdebdf4365422958f453d63) )
	ROM_LOAD32_BYTE("14.bin", 0x0000002, 0x1080000, CRC(67983453) SHA1(05389a0ffc1a1bae9bac16a53a97d78b6eccc626) )
	ROM_LOAD32_BYTE("19.bin", 0x0000003, 0x1080000, CRC(9fc5c579) SHA1(8829329ef229564952aea2108ef1750dc226cbac) )

	ROM_REGION( 0x20000, "fpga", ROMREGION_ERASEFF)
	ROM_LOAD("epc1pc8.ic23", 0x0000000, 0x1ff01, CRC(752444c7) SHA1(c77e8fcfcbe15b53eda25553763bdac45f0ef7df) ) // contains configuration data for the fpga
ROM_END

ROM_START( atvtracka )
	ROM_REGION( 0x4200000, "nand", ROMREGION_ERASEFF) // NAND roms, contain additional data hence the sizes
	ROM_LOAD32_BYTE("k9f2808u0b.ic15", 0x0000000, 0x1080000, CRC(10730001) SHA1(48c685a6ff7135abd074dc7fb7d10834c44da58f) )
	ROM_LOAD32_BYTE("k9f2808u0b.ic20", 0x0000001, 0x1080000, CRC(b0c34433) SHA1(852c79bb3d7082cd2c056140071ae7d71679ec1d) )
	ROM_LOAD32_BYTE("k9f2808u0b.ic14", 0x0000002, 0x1080000, CRC(02a12085) SHA1(acb112c9c7b29d92610465fb92268ce787ca06f4) )
	ROM_LOAD32_BYTE("k9f2808u0b.ic19", 0x0000003, 0x1080000, CRC(856c1e6a) SHA1(a6b2839120d61811c36cc6b4095de9cefceb394b) )

	ROM_REGION( 0x20000, "fpga", ROMREGION_ERASEFF)
	ROM_LOAD("epc1pc8.ic23", 0x0000000, 0x1ff01, CRC(752444c7) SHA1(c77e8fcfcbe15b53eda25553763bdac45f0ef7df) ) // contains configuration data for the fpga
ROM_END

/* Gaelco Football uses a small I/O PCB for connecting the balls (you control the game by kicking a real ball):
    -ADXL250JQC single/dual axis accelerometer.
    -PIC16C710.
    -8L05A linear voltage regulator.
    -UA741C single operational amplifier.
    -16 MHz osc.
*/
ROM_START( gfootbal )
	ROM_REGION( 0x4200000, "nand", ROMREGION_ERASEFF) // NAND roms, contain additional data hence the sizes
	ROM_LOAD32_BYTE("k9f2808u0b.ic15",  0x00000000, 0x01080000, CRC(876ca493) SHA1(d888be59d924fe23e725c6a8aa9609e9abcab608) )
	ROM_LOAD32_BYTE("k9f2808u0b.ic20",  0x00000001, 0x01080000, CRC(df9cf6e2) SHA1(07be171c3768de8f548bceeaf3da8d3bf7cb85d3) )
	ROM_LOAD32_BYTE("k9f2808u0b.ic14",  0x00000002, 0x01080000, CRC(48d901a3) SHA1(00f151b67e5603354a97708247bed46a2f1bbe1d) )
	ROM_LOAD32_BYTE("k9f2808u0b.ic19",  0x00000003, 0x01080000, CRC(2db6c016) SHA1(490d23e338014b4e5f2f12dd04dfae2a93a15e11) )

	ROM_REGION( 0x20000, "fpga", ROMREGION_ERASEFF)
	ROM_LOAD("epc1pc8.ic23", 0x0000000, 0x1ff01, CRC(752444c7) SHA1(c77e8fcfcbe15b53eda25553763bdac45f0ef7df) ) // contains configuration data for the fpga

	ROM_REGION( 0x20000, "io", ROMREGION_ERASEFF)
	ROM_LOAD("4r_pic16c710.u1", 0x0000, 0x2000, NO_DUMP ) // I/O for the ball controller
ROM_END

/*

Smashing Drive
Gaelco 2000

PCB Layout
----------

REF 010131
|----------------------------------------------|
|                                              |
|                              K4S643232C      |
|        |------|SDRB.IC14                     |
|        |SH4   |                              |-|
|        |      |                              | |DB9
|        |      |SDRA.IC15  SDRC.IC20  PRG.IC23|-|
|        |------|             |----------|     |
|                             |ALTERA    |     |
|                             |FLEX0K50  |     |
|        |------|  K4S643232C |EPF10K50  |     |
|        |SH4   |  K4S643232C |EQC240-3  |     |
|        |      |             |          |     |
|        |      |             |          |     |
|        |------|             |----------|     |
|                                              |
|         33MHz                 |--------|     |
|                    K4S643232C |NEC     |     |
|                    K4S643232C |POWERVR |     |
|                               |250     |     |
|                    K4S643232C |        |     |-|
|                    K4S643232C |        |     | |DB9
|                               |--------|     |-|
|                                              |
|  TL074C   TL074C                14.31818MHz  |
|  TDA1543  TDA1543                            |
|----------------------------------------------|

 PRG ROM useful locations:
   0x64C44 Country: 0 - World, 1 - Spain, 2 - UK, 3 - Italy, 4 - USA
   0x727AC 1 - enable Develop/debug option in test mode
*/

ROM_START( smashdrv ) // World Version: 3.3, Version 3D: 1.9, Checksum: 707A
	ROM_REGION64_LE( 0x0400000, "data", ROMREGION_ERASEFF)
	ROM_LOAD("prg_world.ic23", 0x0000000, 0x0400000, CRC(c642b059) SHA1(8a898f46cebc5951a6355f2b51e31ac5e17b4bca) )

	ROM_REGION( 0x4000000, "maincpu", ROMREGION_ERASEFF)
	ROM_LOAD32_WORD("sdra.ic15",    0x00000000, 0x01000000, CRC(cf702287) SHA1(84cd83c339831deff15fe5fcc353e0b596667500) )
	ROM_LOAD32_WORD("sdrb.ic14",    0x00000002, 0x01000000, CRC(39b76f0e) SHA1(529943b6075925e5f72c6e966796e04b2c33686c) )
	ROM_LOAD("sdrc.ic20",          0x02000000, 0x01000000, CRC(c9021dd7) SHA1(1d08aab433614810af858a0fc5d7f03c7b782237) ) // read as plain consecutive bytes (resource file headers line up), not interleaved with the unpopulated ic21
ROM_END

ROM_START( smashdrvs ) // Spain/Portugal Version: 3.3, Version 3D: 1.9, Checksum: 707B. There is another known (undumped) Spain/Portugal (Covielsa license) set with Version: 3.3, Version 3D: 1.9, Checksum: EDD9.
	ROM_REGION64_LE( 0x0400000, "data", ROMREGION_ERASEFF)
	ROM_LOAD("prg_spain.ic23", 0x0000000, 0x0400000, CRC(66b80283) SHA1(7d569670fd96aaa99da24378c77a265bc2ddc91c) )

	ROM_REGION( 0x4000000, "maincpu", ROMREGION_ERASEFF)
	ROM_LOAD32_WORD("sdra.ic15",    0x00000000, 0x01000000, CRC(cf702287) SHA1(84cd83c339831deff15fe5fcc353e0b596667500) )
	ROM_LOAD32_WORD("sdrb.ic14",    0x00000002, 0x01000000, CRC(39b76f0e) SHA1(529943b6075925e5f72c6e966796e04b2c33686c) )
	ROM_LOAD("sdrc.ic20",          0x02000000, 0x01000000, CRC(c9021dd7) SHA1(1d08aab433614810af858a0fc5d7f03c7b782237) ) // read as plain consecutive bytes (resource file headers line up), not interleaved with the unpopulated ic21
ROM_END

ROM_START( smashdrvb ) // UK Version: 3.3, Version 3D: 1.9, Checksum: 707C
	ROM_REGION64_LE( 0x0400000, "data", ROMREGION_ERASEFF)
	ROM_LOAD("prg.ic23", 0x0000000, 0x0400000, CRC(5cc6d3ac) SHA1(0c8426774212d891796b59c95b8c70f64db5b67a) )

	ROM_REGION( 0x4000000, "maincpu", ROMREGION_ERASEFF)
	ROM_LOAD32_WORD("sdra.ic15",    0x00000000, 0x01000000, CRC(cf702287) SHA1(84cd83c339831deff15fe5fcc353e0b596667500) )
	ROM_LOAD32_WORD("sdrb.ic14",    0x00000002, 0x01000000, CRC(39b76f0e) SHA1(529943b6075925e5f72c6e966796e04b2c33686c) )
	ROM_LOAD("sdrc.ic20",          0x02000000, 0x01000000, CRC(c9021dd7) SHA1(1d08aab433614810af858a0fc5d7f03c7b782237) ) // read as plain consecutive bytes (resource file headers line up), not interleaved with the unpopulated ic21
ROM_END

} // anonymous namespace


GAME( 2002, atvtrack,  0,        atvtrack, atvtrack, atvtrack_state, empty_init, ROT0, "Gaelco",           "ATV Track (set 1)", MACHINE_IMPERFECT_GRAPHICS | MACHINE_IMPERFECT_CONTROLS )
GAME( 2002, atvtracka, atvtrack, atvtrack, atvtrack, atvtrack_state, empty_init, ROT0, "Gaelco",           "ATV Track (set 2)", MACHINE_IMPERFECT_GRAPHICS | MACHINE_IMPERFECT_CONTROLS )
GAME( 2002, gfootbal,  0,        atvtrack, atvtrack, atvtrack_state, empty_init, ROT0, "Gaelco / Zigurat", "Gaelco Football",   MACHINE_NOT_WORKING | MACHINE_NO_SOUND )

// almost identical PCB, FlashROM mapping and master registers addresses different
GAME( 2000, smashdrv,  0,        smashdrv, smashdrv, smashdrv_state, empty_init, ROT0, "Gaelco",                       "Smashing Drive (World)",           MACHINE_IMPERFECT_GRAPHICS )
GAME( 2000, smashdrvb, smashdrv, smashdrv, smashdrv, smashdrv_state, empty_init, ROT0, "Gaelco (Brent Sales license)", "Smashing Drive (UK)",              MACHINE_IMPERFECT_GRAPHICS )
GAME( 2000, smashdrvs, smashdrv, smashdrv, smashdrv, smashdrv_state, empty_init, ROT0, "Gaelco (Covielsa license)",    "Smashing Drive (Spain, Portugal)", MACHINE_IMPERFECT_GRAPHICS )
