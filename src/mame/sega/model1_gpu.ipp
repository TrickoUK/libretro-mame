// license:BSD-3-Clause
// copyright-holders:TrickoUK
/***************************************************************************

    Sega Model 1 host-GPU path (included by model1_v.cpp)

    The below-HUD pass (0x01 objects and 0x02 direct polygons) is drawn on
    the host GPU so it can be supersampled and multisampled. The TGP
    simulation, the clipping and the far-to-near sort run on the CPU as in
    software; each sorted viewport batch becomes one run of triangles in
    the same order with the viewport as the scissor. The polygons are flat
    shaded, so the state record carries the colour and the moire flag
    (model1_shader.h).

    The picture comes back averaged to the native size with the drawn
    share of each pixel in alpha, and is blended over the background
    tilemaps. The HUD tilemaps and the above-HUD pass are drawn on top by
    screen_update_model1() as in software.

***************************************************************************/

#include "model1_shader.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <vector>


class model1_renderer
{
public:
	model1_renderer(model1_state &state) : m_state(state) { }
	~model1_renderer();

	bool collecting() const { return m_collecting; }

	// Draws the below-HUD pass into bitmap on the host GPU. Returns false, with nothing drawn, when the GPU path
	// cannot produce the frame; the caller then draws the pass in software.
	bool render_below_hud(bitmap_rgb32 &bitmap, const rectangle &cliprect);

	// Called by draw_quads() while collecting: one sorted batch clipped to the viewport.
	void add_batch(const model1_state::view_t &view, model1_state::quad_t *const *quads, int count);

private:
	struct gpu_vertex
	{
		float x, y, z, w;
		u32 prim;
	};

	struct point2
	{
		double x, y;
	};

	bool init();
	point2 screen_position(const model1_state::point_t &p) const;
	u32 add_state(int color);
	void add_polygon(const point2 *points, int count, u32 prim);
	void add_covering_polygon(const point2 *points, u32 prim);
	void add_line(point2 a, point2 b, u32 prim);

	model1_state &m_state;

	osd::gpu_engine *m_engine = nullptr;
	osd::gpu_handle m_module = osd::GPU_NULL_HANDLE;
	bool m_checked = false;
	bool m_collecting = false;
	int m_scale = 1;

	// the clip rectangle of the current batch, in native pixels (max exclusive)
	int m_clip_x1 = 0, m_clip_y1 = 0, m_clip_x2 = 0, m_clip_y2 = 0;

	std::vector<gpu_vertex> m_vertices;
	std::vector<u32> m_state_words;
	std::vector<osd::gpu_command> m_commands;
	bitmap_rgb32 m_layer;
};


void model1_state::gpu_renderer_deleter::operator()(model1_renderer *renderer) const
{
	delete renderer;
}


model1_renderer::~model1_renderer()
{
	if (m_engine && m_module)
		m_engine->destroy_shader_module(m_module);
}

bool model1_renderer::init()
{
	if (!m_checked)
	{
		m_checked = true;
		m_engine = m_state.machine().osd().get_gpu_engine("model1");
		if (m_engine)
		{
			osd::gpu_shader_module module;
			module.name = "model1";
			module.fragment_source = MODEL1_FRAGMENT_SOURCE;
			module.set_varyings(MODEL1_SHADER_VARYINGS, osd::gpu_varying_mode::NOPERSPECTIVE, false);
			module.state_stride = MODEL1_SHADER_STATE_WORDS;
			m_module = m_engine->create_shader_module(module);
			if (!m_module)
				m_engine = nullptr;
		}
		if (m_engine)
		{
			const rectangle &visarea = m_state.m_screen->visible_area();
			m_layer.allocate(visarea.right() + 1, visarea.bottom() + 1);
		}
	}
	return m_engine && !m_engine->failed();
}

// The screen position of a vertex in native pixels. The scanline filler draws a vertex in the pixel its position
// truncates to; the GPU takes the position before truncation, so edges move in fractions of a pixel. A vertex the
// software leaves at a position its projection did not produce (behind the eye) keeps that position.
model1_renderer::point2 model1_renderer::screen_position(const model1_state::point_t &p) const
{
	if (std::isfinite(p.fx) && std::isfinite(p.fy) && std::fabs(p.fx) < 1.0e9f && std::fabs(p.fy) < 1.0e9f
			&& int32_t(p.fx) == p.s.x && int32_t(p.fy) == p.s.y)
		return { double(p.fx), double(p.fy) };
	return { double(p.s.x) + 0.5, double(p.s.y) + 0.5 };
}

u32 model1_renderer::add_state(int color)
{
	const u32 prim = m_state_words.size() / MODEL1_SHADER_STATE_WORDS;
	m_state_words.push_back(color & 0xffffff);
	m_state_words.push_back((color & MOIRE) ? 1 : 0);
	m_state_words.push_back(m_scale);
	return prim;
}

// Clips a convex polygon (native pixels) to the batch clip rectangle plus a one pixel margin in double precision,
// so vertices far outside the screen keep their precision, and appends it as a triangle fan. The scissor does the
// exact clip.
void model1_renderer::add_polygon(const point2 *points, int count, u32 prim)
{
	point2 poly[2][12];
	int cur = 0;
	std::copy_n(points, count, poly[0]);
	const double limits[4] = { double(m_clip_x1) - 1.0, double(m_clip_x2) + 1.0, double(m_clip_y1) - 1.0, double(m_clip_y2) + 1.0 };
	for (int edge = 0; edge < 4 && count >= 3; edge++)
	{
		const bool horizontal = edge < 2;
		const bool upper = (edge & 1) != 0;
		const double limit = limits[edge];
		auto inside = [&] (const point2 &v)
		{
			const double c = horizontal ? v.x : v.y;
			return upper ? (c <= limit) : (c >= limit);
		};
		const point2 *const in = poly[cur];
		point2 *const out = poly[cur ^ 1];
		int out_count = 0;
		for (int i = 0; i < count; i++)
		{
			const point2 &a = in[i];
			const point2 &b = in[(i + 1) % count];
			const bool a_in = inside(a);
			const bool b_in = inside(b);
			if (a_in)
				out[out_count++] = a;
			if (a_in != b_in)
			{
				const double ca = horizontal ? a.x : a.y;
				const double cb = horizontal ? b.x : b.y;
				const double t = (limit - ca) / (cb - ca);
				point2 &n = out[out_count++];
				n.x = horizontal ? limit : (a.x + (b.x - a.x) * t);
				n.y = horizontal ? (a.y + (b.y - a.y) * t) : limit;
			}
		}
		count = out_count;
		cur ^= 1;
	}
	if (count < 3)
		return;

	const float scale = m_scale;
	auto vertex = [&] (const point2 &p) { return gpu_vertex{ float(p.x) * scale, float(p.y) * scale, 0.0f, 1.0f, prim }; };
	for (int i = 1; i + 1 < count; i++)
	{
		m_vertices.push_back(vertex(poly[cur][0]));
		m_vertices.push_back(vertex(poly[cur][i]));
		m_vertices.push_back(vertex(poly[cur][i + 1]));
	}
}

// A line covers one pixel across its minor axis at every pixel step along its major axis, from the pixel of one end
// to the pixel of the other, like the Bresenham walk: a parallelogram one native pixel thick on the minor axis,
// extended by half a pixel past each end. The small shift puts a position on a pixel boundary into the pixel it
// truncates to.
void model1_renderer::add_line(point2 a, point2 b, u32 prim)
{
	constexpr double BIAS = 1.0 / 256.0;
	a.x += BIAS;
	a.y += BIAS;
	b.x += BIAS;
	b.y += BIAS;
	const double dx = b.x - a.x;
	const double dy = b.y - a.y;
	point2 quad[4];
	if (std::fabs(dx) >= std::fabs(dy))
	{
		if (dx < 0)
			std::swap(a, b);
		const double slope = (b.x != a.x) ? ((b.y - a.y) / (b.x - a.x)) : 0.0;
		const point2 s{ a.x - 0.5, a.y - 0.5 * slope };
		const point2 e{ b.x + 0.5, b.y + 0.5 * slope };
		quad[0] = { s.x, s.y - 0.5 };
		quad[1] = { e.x, e.y - 0.5 };
		quad[2] = { e.x, e.y + 0.5 };
		quad[3] = { s.x, s.y + 0.5 };
	}
	else
	{
		if (dy < 0)
			std::swap(a, b);
		const double slope = (b.x - a.x) / (b.y - a.y);
		const point2 s{ a.x - 0.5 * slope, a.y - 0.5 };
		const point2 e{ b.x + 0.5 * slope, b.y + 0.5 };
		quad[0] = { s.x - 0.5, s.y };
		quad[1] = { s.x + 0.5, s.y };
		quad[2] = { e.x + 0.5, e.y };
		quad[3] = { e.x - 0.5, e.y };
	}
	add_polygon(quad, 4, prim);
}

// The scanline filler draws every pixel a quad touches: its spans run from the truncated left edge to the truncated
// right edge inclusive, on every row from the truncated top to the truncated bottom. The GPU covers pixel centres,
// so the quad is grown by half a native pixel on each side (the convex hull of its corners moved to the corners of
// a pixel-sized square) to cover the same pixels; thin strips keep their width and supersampling smooths the edges
// of the grown outline.
void model1_renderer::add_covering_polygon(const point2 *points, u32 prim)
{
	point2 cand[16];
	for (int i = 0; i < 4; i++)
	{
		cand[i * 4 + 0] = { points[i].x - 0.5, points[i].y - 0.5 };
		cand[i * 4 + 1] = { points[i].x + 0.5, points[i].y - 0.5 };
		cand[i * 4 + 2] = { points[i].x + 0.5, points[i].y + 0.5 };
		cand[i * 4 + 3] = { points[i].x - 0.5, points[i].y + 0.5 };
	}
	std::sort(std::begin(cand), std::end(cand), [] (const point2 &a, const point2 &b) { return (a.x < b.x) || (a.x == b.x && a.y < b.y); });

	// monotone chain convex hull, counter-clockwise without collinear points
	auto cross = [] (const point2 &o, const point2 &a, const point2 &b) { return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x); };
	point2 hull[32];
	int n = 0;
	for (int i = 0; i < 16; i++)
	{
		while (n >= 2 && cross(hull[n - 2], hull[n - 1], cand[i]) <= 0)
			n--;
		hull[n++] = cand[i];
	}
	for (int i = 14, lower = n + 1; i >= 0; i--)
	{
		while (n >= lower && cross(hull[n - 2], hull[n - 1], cand[i]) <= 0)
			n--;
		hull[n++] = cand[i];
	}
	n--; // the last point repeats the first
	if (n >= 3)
		add_polygon(hull, n, prim);
}

void model1_renderer::add_batch(const model1_state::view_t &view, model1_state::quad_t *const *quads, int count)
{
	if (count <= 0 || view.x1 > view.x2 || view.y1 > view.y2)
		return;

	m_clip_x1 = view.x1;
	m_clip_y1 = view.y1;
	m_clip_x2 = view.x2 + 1;
	m_clip_y2 = view.y2 + 1;
	const int sx = m_clip_x1 * m_scale;
	const int sy = m_clip_y1 * m_scale;
	const int sw = (m_clip_x2 - m_clip_x1) * m_scale;
	const int sh = (m_clip_y2 - m_clip_y1) * m_scale;
	const osd::gpu_command *last_clip = nullptr;
	for (auto it = m_commands.rbegin(); it != m_commands.rend(); ++it)
	{
		if (it->type == osd::gpu_command_type::SET_CLIP)
		{
			last_clip = &*it;
			break;
		}
	}
	if (!last_clip || last_clip->x != sx || last_clip->y != sy || last_clip->w != sw || last_clip->h != sh)
		m_commands.push_back(osd::gpu_command::make_clip(sx, sy, sw, sh));

	const u32 first = m_vertices.size();
	for (int i = 0; i < count; i++)
	{
		const model1_state::quad_t &q = *quads[i];
		int color = q.col;
		if (color < 0)
			color = -1 - color;

		// The same distinct-point test as fill_quad(): two distinct points make a wireframe line, all points on one
		// row make a single span (or a single pixel).
		const int ax = q.p[0]->s.x, ay = q.p[0]->s.y;
		int bx = ax, by = ay, ndist = 1;
		bool one_row = true;
		for (int j = 1; j < 4; j++)
		{
			const int vx = q.p[j]->s.x, vy = q.p[j]->s.y;
			if (vy != ay)
				one_row = false;
			if (ndist == 3 || (vx == ax && vy == ay))
				continue;
			if (ndist == 1)
			{
				bx = vx;
				by = vy;
				ndist = 2;
			}
			else if (vx != bx || vy != by)
			{
				ndist = 3;
			}
		}

		point2 pts[4];
		for (int j = 0; j < 4; j++)
			pts[j] = screen_position(*q.p[j]);

		const u32 prim = add_state(color);
		if (ndist == 2)
		{
			int k = 1;
			while (q.p[k]->s.x == ax && q.p[k]->s.y == ay)
				k++;
			add_line(pts[0], pts[k], prim);
		}
		else if (one_row)
		{
			int left = 0, right = 0;
			for (int j = 1; j < 4; j++)
			{
				if (pts[j].x < pts[left].x)
					left = j;
				if (pts[j].x > pts[right].x)
					right = j;
			}
			add_line(pts[left], pts[right], prim);
		}
		else
		{
			add_covering_polygon(pts, prim);
		}
	}

	const u32 added = m_vertices.size() - first;
	if (added == 0)
		return;
	if (!m_commands.empty() && m_commands.back().type == osd::gpu_command_type::DRAW)
	{
		m_commands.back().vertex_count += added;
	}
	else
	{
		osd::gpu_command draw = osd::gpu_command::make_draw(m_module, first, added);
		m_commands.push_back(draw);
	}
}

bool model1_renderer::render_below_hud(bitmap_rgb32 &bitmap, const rectangle &cliprect)
{
	if (!init())
		return false;

	const int width = m_layer.width();
	const int height = m_layer.height();
	m_scale = osd::gpu_pick_scale(m_state.machine().osd().supersample(), std::max(width, height), m_engine->caps().max_texture_size);

	m_vertices.clear();
	m_state_words.clear();
	m_commands.clear();

	m_collecting = true;
	m_state.tgp_render(bitmap, cliprect, model1_state::RENDER_BELOW_HUD);
	m_collecting = false;

	if (m_vertices.empty())
		return true;

	const rectangle clip = cliprect & rectangle(0, width - 1, 0, height - 1);
	const int msaa = m_state.machine().osd().msaa();

	// The layer is transparent where nothing was drawn, so an edge pixel averages its drawn samples only and
	// carries the drawn share in alpha.
	osd::gpu_readback_rect readback;
	readback.x = clip.left() * m_scale;
	readback.y = clip.top() * m_scale;
	readback.w = clip.width() * m_scale;
	readback.h = clip.height() * m_scale;
	readback.out = &m_layer.pix(clip.top(), clip.left());
	readback.stride = m_layer.rowpixels();
	if (m_scale > 1 || msaa > 0)
	{
		readback.downsample = m_scale;
		readback.coverage = true;
		readback.coverage_share = true;
	}

	osd::gpu_frame frame;
	frame.target.width = width * m_scale;
	frame.target.height = height * m_scale;
	frame.target.depth = osd::gpu_depth_mode::NONE;
	frame.target.msaa_samples = msaa;
	frame.target.msaa_per_sample = msaa > 0;
	frame.vertices = m_vertices.data();
	frame.vertex_count = m_vertices.size();
	frame.varyings_per_vertex = MODEL1_SHADER_VARYINGS;
	frame.prim_state = m_state_words.data();
	frame.prim_count = m_state_words.size() / MODEL1_SHADER_STATE_WORDS;
	frame.state_stride = MODEL1_SHADER_STATE_WORDS;
	frame.commands = m_commands.data();
	frame.command_count = m_commands.size();
	frame.readbacks = &readback;
	frame.readback_count = 1;
	if (!m_engine->submit_frame(frame))
		return false;

	// blend the layer over the background tilemaps by the drawn share
	for (int y = clip.top(); y <= clip.bottom(); y++)
	{
		u32 const *const src = &m_layer.pix(y);
		u32 *const dst = &bitmap.pix(y);
		for (int x = clip.left(); x <= clip.right(); x++)
		{
			const u32 s = src[x];
			const u32 a = s >> 24;
			if (a == 0)
				continue;
			if (a == 0xff)
			{
				dst[x] = s & 0xffffff;
				continue;
			}
			const u32 d = dst[x];
			const u32 r = (((s >> 16) & 0xff) * a + ((d >> 16) & 0xff) * (0xff - a) + 0x7f) / 0xff;
			const u32 g = (((s >> 8) & 0xff) * a + ((d >> 8) & 0xff) * (0xff - a) + 0x7f) / 0xff;
			const u32 b = ((s & 0xff) * a + (d & 0xff) * (0xff - a) + 0x7f) / 0xff;
			dst[x] = (r << 16) | (g << 8) | b;
		}
	}
	return true;
}
