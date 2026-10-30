// license:BSD-3-Clause
// copyright-holders:TrickoUK
/***************************************************************************

    Midway V-Unit host-GPU path (included by midvunit_v.cpp)

    The quads that process_dma_queue() would hand to the scanline routines
    become one triangle list in the same order, with a state record per quad
    for the colour rules of the chip. The frame buffer is one persistent
    target holding both 512 x 512 index pages, one above the other. The
    colour of each index is looked up in the palette when the quad is drawn,
    so every palette write and texture RAM write is put into the command list
    at the point where the CPU made it, and the quads after it see it. The
    pixels of the page on screen come back as ordinary pixels when the screen
    is updated.

***************************************************************************/

bool midvunit_renderer::gpu_init()
{
	m_gpu_checked = true;
	m_gpu_engine = m_state.machine().osd().get_gpu_engine("midvunit");
	if (!m_gpu_engine)
		return false;
	if (m_gpu_engine->caps().max_texture_size < GPU_TEXTURE_WIDTH)
	{
		osd_printf_verbose("midvunit: host GPU path unavailable, the largest texture is %d texels and %d are needed\n", m_gpu_engine->caps().max_texture_size, GPU_TEXTURE_WIDTH);
		m_gpu_engine = nullptr;
		return false;
	}

	// the options are read when the machine starts, as the core option text says they take effect on a restart
	m_gpu_msaa = m_state.machine().osd().msaa();
	m_gpu_texfilter = m_state.machine().osd().texture_filter();
	m_gpu_scale = osd::gpu_pick_scale(m_state.machine().osd().supersample(), GPU_TARGET_HEIGHT, int(m_gpu_engine->caps().max_texture_size));

	// a sample is taken inside the quad for a pixel the quad only partly covers, so u and v are never extrapolated past its edges
	osd::gpu_shader_module module;
	module.name = "midvunit";
	module.fragment_source = MIDVUNIT_FRAGMENT_SOURCE;
	module.set_varyings(MIDVUNIT_SHADER_VARYINGS, osd::gpu_varying_mode::NOPERSPECTIVE, m_gpu_msaa > 0);
	module.state_stride = MIDVUNIT_SHADER_STATE_WORDS;
	module.binding_count = 2;
	module.binding_name[0] = "tex_ram";
	module.binding_format[0] = osd::gpu_texture_format::R8UI;
	module.binding_name[1] = "palette";
	module.binding_format[1] = osd::gpu_texture_format::RGBA8;
	m_gpu_module = m_gpu_engine->create_shader_module(module);

	osd::gpu_texture_desc desc;
	desc.format = osd::gpu_texture_format::R8UI;
	desc.width = GPU_TEXTURE_WIDTH;
	desc.height = GPU_TEXTURE_HEIGHT;
	m_gpu_texture = m_gpu_engine->create_texture(desc);
	desc.format = osd::gpu_texture_format::RGBA8;
	desc.width = 256;
	desc.height = GPU_PALETTE_ENTRIES / 256;
	m_gpu_palette = m_gpu_engine->create_texture(desc);

	if (!m_gpu_module || !m_gpu_texture || !m_gpu_palette)
	{
		m_gpu_engine = nullptr;
		return false;
	}

	m_gpu_shadow_palette = std::make_unique<uint32_t[]>(GPU_PALETTE_ENTRIES);
	m_gpu_ready = true;
	gpu_reset();
	osd_printf_info("midvunit: host GPU path on, target %dx%d, %dx supersampling, %d MSAA samples, bilinear %s\n",
			GPU_PAGE_WIDTH * m_gpu_scale, GPU_TARGET_HEIGHT * m_gpu_scale, m_gpu_scale, m_gpu_msaa, m_gpu_texfilter ? "on" : "off");
	return true;
}

// Called when the frame buffer state the target holds is not valid: a state load, a reset, or the
// first use. The target is cleared at the next frame, and the texture RAM and the palette are uploaded again in full.
void midvunit_renderer::gpu_reset()
{
	m_gpu_generation++;
	m_gpu_palette_full = true;
	m_gpu_texture_lo = 0;
	m_gpu_texture_hi = GPU_TEXTURE_WIDTH * GPU_TEXTURE_HEIGHT;
	m_gpu_clip_page = -1;
	m_gpu_commands.clear();
	m_gpu_vertices.clear();
	m_gpu_state.clear();
	m_gpu_blobs.clear();
}

// The target does not match the CPU's view of the frame buffer (the CPU read or wrote it, which the adapter does not
// mirror). The rest of the session runs in software. Quads queued for the frame that was being built are dropped.
void midvunit_renderer::gpu_latch_off(const char *reason)
{
	if (!m_gpu_ready)
		return;
	osd_printf_verbose("midvunit: host GPU path off for the rest of the session: %s\n", reason);
	m_gpu_ready = false;
	m_gpu_engine = nullptr;
	m_gpu_commands.clear();
	m_gpu_vertices.clear();
	m_gpu_state.clear();
	m_gpu_blobs.clear();
	m_gpu_clip_page = -1;
}

// A CPU write to texture RAM. The bytes are uploaded before the next quad, from the RAM as it is at that point.
void midvunit_renderer::gpu_mark_texture(uint32_t offset, uint32_t bytes)
{
	if (m_gpu_texture_hi <= m_gpu_texture_lo)
	{
		m_gpu_texture_lo = offset;
		m_gpu_texture_hi = offset + bytes;
	}
	else
	{
		m_gpu_texture_lo = std::min(m_gpu_texture_lo, offset);
		m_gpu_texture_hi = std::max(m_gpu_texture_hi, offset + bytes);
	}
}

// A CPU write to palette RAM. A changed entry is put into the command list, so the quads drawn after it resolve their
// colours through it. Quads drawn before it keep the colour they were drawn with.
void midvunit_renderer::gpu_palette_write(uint32_t index)
{
	if (!m_gpu_ready)
		return;
	const uint32_t pen = m_palette.pen(index);
	if (m_gpu_shadow_palette[index] == pen)
		return;
	m_gpu_shadow_palette[index] = pen;

	auto &blob = m_gpu_blobs.emplace_back(1);
	blob[0] = pen;
	osd::gpu_texture_update update;
	update.texture = m_gpu_palette;
	update.x = index & 255;
	update.y = index >> 8;
	update.w = 1;
	update.h = 1;
	update.data = blob.data();
	m_gpu_commands.push_back(osd::gpu_command::make_update(update));
}

// Puts the pending texture RAM bytes and, after a reset, the whole palette into the command list. Called before each quad,
// so a quad always sees the RAM and palette as they stood when the CPU triggered it.
void midvunit_renderer::gpu_flush_updates()
{
	if (m_gpu_palette_full)
	{
		m_gpu_palette_full = false;
		auto &blob = m_gpu_blobs.emplace_back(GPU_PALETTE_ENTRIES);
		for (int i = 0; i < GPU_PALETTE_ENTRIES; i++)
		{
			const uint32_t pen = m_palette.pen(i);
			m_gpu_shadow_palette[i] = pen;
			blob[i] = pen;
		}
		osd::gpu_texture_update update;
		update.texture = m_gpu_palette;
		update.w = 256;
		update.h = GPU_PALETTE_ENTRIES / 256;
		update.data = blob.data();
		m_gpu_commands.push_back(osd::gpu_command::make_update(update));
	}

	if (m_gpu_texture_hi > m_gpu_texture_lo)
	{
		// whole rows, from the first to the last one touched
		const uint32_t first_row = m_gpu_texture_lo / GPU_TEXTURE_WIDTH;
		const uint32_t rows = (m_gpu_texture_hi - 1) / GPU_TEXTURE_WIDTH - first_row + 1;
		auto &blob = m_gpu_blobs.emplace_back(rows * GPU_TEXTURE_WIDTH / 4);
		const uint8_t *const ram = (const uint8_t *)m_state.m_textureram.target();
		memcpy(blob.data(), ram + first_row * GPU_TEXTURE_WIDTH, rows * GPU_TEXTURE_WIDTH);
		osd::gpu_texture_update update;
		update.texture = m_gpu_texture;
		update.y = first_row;
		update.w = GPU_TEXTURE_WIDTH;
		update.h = rows;
		update.data = blob.data();
		m_gpu_commands.push_back(osd::gpu_command::make_update(update));
		m_gpu_texture_lo = 0;
		m_gpu_texture_hi = 0;
	}
}

// The rounding of poly_manager::round_coordinate() (private there): the nearest integer, a half rounds down.
static int32_t gpu_round(float value)
{
	const float ipart = std::floor(value);
	return int32_t(ipart) + (((value - ipart) > 0.5f) ? 1 : 0);
}

// Adds one quad the way the scanline renderer would draw it: the same vertices (already made inclusive), the same
// palette base and mode. The two triangles split the quad along the diagonal from vertex 0 to vertex 2. Bit n of nudge_x
// and nudge_y marks the vertex n whose x or y make_vertices_inclusive moved to include the right or bottom edge.
void midvunit_renderer::gpu_add_quad(const vertex_t *vert, uint8_t nudge_x, uint8_t nudge_y, uint16_t pixdata, bool textured, uint16_t flags, uint16_t texpage)
{
	// the modes of the chip's textured quads (flags bits 10-11), as the shader's state words
	constexpr uint32_t MODE_TEX = 0;
	constexpr uint32_t MODE_TEXTRANS = 1;
	constexpr uint32_t MODE_TEXTRANSMASK = 2;
	constexpr uint32_t MODE_FLAT = 3;
	uint32_t mode = MODE_FLAT;
	if (textured)
	{
		switch (flags & 0xc00)
		{
		case 0x000: mode = MODE_TEX; break;
		case 0x800: mode = MODE_TEXTRANS; break;
		case 0xc00: mode = MODE_TEXTRANSMASK; break;
		default:    mode = MODE_FLAT; break;
		}
	}

	gpu_flush_updates();

	// the draw page is stacked below the display page's rows of the target; the target holds m_gpu_scale samples of each native pixel
	const int page = (m_state.m_page_control & 4) ? GPU_PAGE_ROWS : 0;
	const float scale = float(m_gpu_scale);
	if (page != m_gpu_clip_page)
	{
		const rectangle &vis = m_state.m_screen->visible_area();
		m_gpu_commands.push_back(osd::gpu_command::make_clip(vis.min_x * m_gpu_scale, (page + vis.min_y) * m_gpu_scale, vis.width() * m_gpu_scale, vis.height() * m_gpu_scale));
		m_gpu_clip_page = page;
	}

	const uint32_t prim = m_gpu_state.size() / MIDVUNIT_SHADER_STATE_WORDS;
	m_gpu_state.push_back(mode);
	m_gpu_state.push_back(pixdata);
	m_gpu_state.push_back(textured ? uint32_t(texpage) * 256 : 0);
	m_gpu_state.push_back((flags & 0x2000) != 0);
	m_gpu_state.push_back(m_gpu_scale);
	m_gpu_state.push_back(m_gpu_texfilter && textured);

	// The chip covers a pixel when the quad contains its centre, so an upright rectangle always covers whole pixels. On a
	// supersampled target its edges are moved to the pixel edges the walker's rounding gives, with the texture coordinates
	// carried along the rectangle's gradient; otherwise an edge on a pixel centre covers half of the pixel's samples.
	float px[4], py[4], pu[4], pv[4];
	for (int i = 0; i < 4; i++)
	{
		px[i] = vert[i].x;
		py[i] = vert[i].y;
		pu[i] = vert[i].p[0];
		pv[i] = vert[i].p[1];
	}
	bool snapped = false;
	if (m_gpu_scale > 1)
	{
		constexpr float TOLERANCE = 0.01f;
		const auto same = [](float a, float b) { return std::abs(a - b) < TOLERANCE; };
		const bool upright = (same(vert[0].x, vert[1].x) && same(vert[2].x, vert[3].x) && same(vert[1].y, vert[2].y) && same(vert[3].y, vert[0].y))
				|| (same(vert[0].y, vert[1].y) && same(vert[2].y, vert[3].y) && same(vert[1].x, vert[2].x) && same(vert[3].x, vert[0].x));
		if (upright)
		{
			for (int i = 0; i < 4; i++)
			{
				// the walker steps the parameters along the two edges at a corner, so each corner is carried by its horizontal
				// neighbour in x and its vertical neighbour in y
				const int next = (i + 1) & 3;
				const int prev = (i + 3) & 3;
				const vertex_t &horizontal = same(vert[i].y, vert[next].y) ? vert[next] : vert[prev];
				const vertex_t &vertical = same(vert[i].y, vert[next].y) ? vert[prev] : vert[next];
				const float dx = horizontal.x - vert[i].x;
				const float dy = vertical.y - vert[i].y;
				const float sx = float(gpu_round(vert[i].x));
				const float sy = float(gpu_round(vert[i].y));
				for (int p = 0; p < 2; p++)
				{
					const float step_x = (std::abs(dx) > TOLERANCE) ? (horizontal.p[p] - vert[i].p[p]) / dx : 0.0f;
					const float step_y = (std::abs(dy) > TOLERANCE) ? (vertical.p[p] - vert[i].p[p]) / dy : 0.0f;
					(p == 0 ? pu : pv)[i] += step_x * (sx - vert[i].x) + step_y * (sy - vert[i].y);
				}
				px[i] = sx;
				py[i] = sy;
			}
			snapped = true;
		}
	}

	// the texture coordinates are 16.16 values with the half texel offset; the shader takes them in texel units
	gpu_vertex vv[4];
	for (int i = 0; i < 4; i++)
	{
		// the walker includes right and bottom pixel centres by moving those vertices 0.001 pixel (make_vertices_inclusive);
		// the device's sub-pixel grid is coarser than that, so the same move is applied here at the device's resolution
		// to the flagged coordinates, which keeps the edge on the pixel centres
		float x = px[i];
		float y = py[i];
		if (!snapped && BIT(nudge_x, i))
			x += GPU_EDGE_NUDGE;
		if (!snapped && BIT(nudge_y, i))
			y += GPU_EDGE_NUDGE;
		vv[i].x = x * scale;
		vv[i].y = (y + page) * scale;
		vv[i].z = 0.0f;
		vv[i].w = 1.0f;
		vv[i].prim = prim;
		vv[i].v[0] = textured ? pu[i] / 65536.0f : 0.0f;
		vv[i].v[1] = textured ? pv[i] / 65536.0f : 0.0f;
	}

	// the range of texture coordinates, in texel units, that the quad's own vertices span
	{
		float umin = vert[0].p[0], umax = vert[0].p[0], vmin = vert[0].p[1], vmax = vert[0].p[1];
		for (int i = 1; i < 4; i++)
		{
			umin = std::min(umin, vert[i].p[0]);
			umax = std::max(umax, vert[i].p[0]);
			vmin = std::min(vmin, vert[i].p[1]);
			vmax = std::max(vmax, vert[i].p[1]);
		}
		for (const float bound : { umin, vmin, umax, vmax })
			m_gpu_state.push_back(std::bit_cast<uint32_t>(textured ? bound / 65536.0f : 0.0f));
	}

	const uint32_t first = m_gpu_vertices.size();
	for (int corner : { 0, 1, 2, 0, 2, 3 })
		m_gpu_vertices.push_back(vv[corner]);

	// consecutive quads with nothing between them share one draw
	if (!m_gpu_commands.empty() && m_gpu_commands.back().type == osd::gpu_command_type::DRAW
			&& m_gpu_commands.back().first_vertex + m_gpu_commands.back().vertex_count == first)
	{
		m_gpu_commands.back().vertex_count += 6;
	}
	else
	{
		osd::gpu_command draw = osd::gpu_command::make_draw(m_gpu_module, first, 6);
		draw.bindings[0] = m_gpu_texture;
		draw.bindings[1] = m_gpu_palette;
		m_gpu_commands.push_back(draw);
	}
}

// The span one scanline of a quad covers, from the edges of poly_manager::render_polygon(): the left and right edges are the
// two chains from the lowest to the highest vertex, and the pixel span of a row runs between their positions at the centre
// of the row, rounded and clipped. The parameters are interpolated linearly along each edge and then across the row. Returns
// false when the row is outside the quad or the clip, or when the span is empty.
bool midvunit_renderer::gpu_row_span(const vertex_t *vert, int32_t row, const rectangle &clip, int32_t &x0, int32_t &x1, float (&param)[2], float (&dpdx)[2]) const
{
	struct edge_t
	{
		const vertex_t *v1;
		const vertex_t *v2;
		float dxdy;
		float dpdy[2];
	};

	// the top and bottom vertices, picked as render_polygon() does
	int minv = 0;
	int maxv = 0;
	for (int vnum = 1; vnum < 4; vnum++)
	{
		if (vert[vnum].y < vert[minv].y)
			minv = vnum;
		else if (vert[vnum].y > vert[maxv].y)
			maxv = vnum;
	}
	const int32_t miny = std::max<int32_t>(gpu_round(vert[minv].y), clip.top());
	const int32_t maxy = std::min<int32_t>(gpu_round(vert[maxv].y), clip.bottom() + 1);
	if (row < miny || row >= maxy)
		return false;

	// the two chains from the top vertex to the bottom vertex, forward and backward, horizontal edges left out
	edge_t fedge[3];
	edge_t bedge[3];
	int nfedge = 0;
	int nbedge = 0;
	for (int curv = minv; curv != maxv; curv = (curv == 3) ? 0 : (curv + 1))
	{
		const vertex_t *const v1 = &vert[curv];
		const vertex_t *const v2 = &vert[(curv == 3) ? 0 : (curv + 1)];
		if (v1->y == v2->y)
			continue;
		const float ooy = 1.0f / (v2->y - v1->y);
		edge_t &edge = fedge[nfedge++];
		edge.v1 = v1;
		edge.v2 = v2;
		edge.dxdy = (v2->x - v1->x) * ooy;
		for (int p = 0; p < 2; p++)
			edge.dpdy[p] = (v2->p[p] - v1->p[p]) * ooy;
	}
	for (int curv = minv; curv != maxv; curv = (curv == 0) ? 3 : (curv - 1))
	{
		const vertex_t *const v1 = &vert[curv];
		const vertex_t *const v2 = &vert[(curv == 0) ? 3 : (curv - 1)];
		if (v1->y == v2->y)
			continue;
		const float ooy = 1.0f / (v2->y - v1->y);
		edge_t &edge = bedge[nbedge++];
		edge.v1 = v1;
		edge.v2 = v2;
		edge.dxdy = (v2->x - v1->x) * ooy;
		for (int p = 0; p < 2; p++)
			edge.dpdy[p] = (v2->p[p] - v1->p[p]) * ooy;
	}
	if (nfedge == 0 || nbedge == 0)
		return false;

	// the left chain is the one whose first edge leaves the top vertex with the smaller slope, or starts further left
	const bool forward_left = (fedge[0].v1 == bedge[0].v1 && fedge[0].dxdy < bedge[0].dxdy) ||
			(fedge[0].v1 != bedge[0].v1 && fedge[0].v1->x < bedge[0].v1->x);
	const edge_t *const ledges = forward_left ? fedge : bedge;
	const edge_t *const redges = forward_left ? bedge : fedge;
	const int nledges = forward_left ? nfedge : nbedge;
	const int nredges = forward_left ? nbedge : nfedge;

	// the edge in force at the centre of the row, moving along each chain as render_polygon() does
	const float fully = float(row) + 0.5f;
	int li = 0;
	int ri = 0;
	while (li + 1 < nledges && fully > ledges[li].v2->y && fully < vert[maxv].y)
		li++;
	while (ri + 1 < nredges && fully > redges[ri].v2->y && fully < vert[maxv].y)
		ri++;
	const edge_t &ledge = ledges[li];
	const edge_t &redge = redges[ri];

	const float startx = ledge.v1->x + (fully - ledge.v1->y) * ledge.dxdy;
	const float stopx = redge.v1->x + (fully - redge.v1->y) * redge.dxdy;
	int32_t istartx = gpu_round(startx);
	int32_t istopx = gpu_round(stopx);
	if (istartx > istopx)
		std::swap(istartx, istopx);
	istartx = std::max(istartx, clip.left());
	istopx = std::min(istopx, clip.right() + 1);
	if (istartx >= istopx)
		return false;

	// the parameters at the first pixel centre of the span, and their step along the row
	const float ldy = fully - ledge.v1->y;
	const float rdy = fully - redge.v1->y;
	const float oox = 1.0f / (stopx - startx);
	for (int p = 0; p < 2; p++)
	{
		const float lparam = ledge.v1->p[p] + ldy * ledge.dpdy[p];
		const float rparam = redge.v1->p[p] + rdy * redge.dpdy[p];
		dpdx[p] = (rparam - lparam) * oox;
		param[p] = lparam + (float(istartx) + 0.5f - startx) * dpdx[p];
	}
	x0 = istartx;
	x1 = istopx;
	return true;
}

// The device rasterizes positions to a sub-pixel grid no finer than GPU_SLIVER_HEIGHT. A quad is drawn row by row, as the
// scanline walker draws it, when it is shorter than that grid, or when one of its nearly horizontal edges (less than that
// grid in height, not exactly horizontal) has an end within the grid of a pixel-centre row. Such an edge snaps onto the
// centre row on the device, where the rasterizer's tie rule differs from the walker's rule at the centre. On a supersampled
// target only the first case is drawn row by row: the rows of a neighbouring quad are whole pixels, which do not meet the
// sloped edge of a quad drawn as it is, and the gap shows what is behind.
bool midvunit_renderer::gpu_needs_rows(const vertex_t *vert) const
{
	float top = vert[0].y;
	float bottom = vert[0].y;
	for (int vnum = 1; vnum < 4; vnum++)
	{
		top = std::min(top, vert[vnum].y);
		bottom = std::max(bottom, vert[vnum].y);
	}
	if (bottom - top < GPU_SLIVER_HEIGHT)
		return true;

	if (m_gpu_scale > 1)
		return false;

	for (int vnum = 0; vnum < 4; vnum++)
	{
		const vertex_t &v1 = vert[vnum];
		const vertex_t &v2 = vert[(vnum + 1) & 3];
		const float dy = std::abs(v2.y - v1.y);
		if (dy == 0.0f || dy >= GPU_SLIVER_HEIGHT)
			continue;
		for (const float y : { v1.y, v2.y })
		{
			if (std::abs(y - std::floor(y) - 0.5f) < GPU_SLIVER_HEIGHT)
				return true;
		}
	}
	return false;
}

// Each row of the quad covered by the scanline walker is drawn as a one-pixel-tall quad over the span that the walker gives
// that row, with the texture coordinates interpolated across it as the walker does.
void midvunit_renderer::gpu_add_rows(const vertex_t *vert, uint16_t pixdata, bool textured, uint16_t flags, uint16_t texpage)
{
	const rectangle &clip = m_state.m_screen->visible_area();
	float top = vert[0].y;
	float bottom = vert[0].y;
	for (int vnum = 1; vnum < 4; vnum++)
	{
		top = std::min(top, vert[vnum].y);
		bottom = std::max(bottom, vert[vnum].y);
	}

	for (int32_t row = gpu_round(top); row <= gpu_round(bottom); row++)
	{
		int32_t x0, x1;
		float param[2], dpdx[2];
		if (!gpu_row_span(vert, row, clip, x0, x1, param, dpdx))
			continue;

		// the parameters at the span's ends, from the centre-of-pixel values: a value at x is param + (x - (x0 + 0.5)) * dpdx
		vertex_t span[4];
		for (int p = 0; p < 2; p++)
		{
			const float left = param[p] - 0.5f * dpdx[p];
			const float right = param[p] + (float(x1 - x0) - 0.5f) * dpdx[p];
			span[0].p[p] = left;
			span[1].p[p] = right;
			span[2].p[p] = right;
			span[3].p[p] = left;
		}
		span[0].x = float(x0);
		span[0].y = float(row);
		span[1].x = float(x1);
		span[1].y = float(row);
		span[2].x = float(x1);
		span[2].y = float(row + 1);
		span[3].x = float(x0);
		span[3].y = float(row + 1);
		// the span vertices are on whole pixels and take no nudge
		gpu_add_quad(span, 0, 0, pixdata, textured, flags, texpage);
	}
}

// Draws the frame's quads into the target and, when the display page changed, reads the page on screen back into the bitmap.
// Returns true when the bitmap holds a new picture. On a refusal by the engine the path is latched off and the picture
// of this frame is left as it was; the next frame is drawn in software.
bool midvunit_renderer::gpu_screen_update(bitmap_rgb32 &bitmap, bool changed)
{
	const rectangle &vis = m_state.m_screen->visible_area();
	if (!m_gpu_ready)
		return false;

	// video_control_w() can declare a visible area larger than one page of the target; the target cannot show it
	if (vis.max_x >= GPU_PAGE_WIDTH || vis.max_y >= GPU_PAGE_ROWS || bitmap.width() <= vis.max_x || bitmap.height() <= vis.max_y)
	{
		gpu_latch_off("visible area larger than a target page");
		m_state.m_video_changed = true;
		return false;
	}

	// the display page starts at the second page of the target when the display bit is set; the read-back averages each
	// native pixel's samples down to one output pixel, and a multisampled target is resolved by the same read-back
	const int display = (m_state.m_page_control & 1) ? GPU_PAGE_ROWS : 0;
	osd::gpu_readback_rect readback;
	readback.x = vis.min_x * m_gpu_scale;
	readback.y = (display + vis.min_y) * m_gpu_scale;
	readback.w = vis.width() * m_gpu_scale;
	readback.h = vis.height() * m_gpu_scale;
	readback.out = &bitmap.pix(vis.min_y, vis.min_x);
	readback.stride = bitmap.rowpixels();
	readback.downsample = m_gpu_scale;
	readback.opaque = true;

	osd::gpu_frame frame;
	frame.target.width = GPU_PAGE_WIDTH * m_gpu_scale;
	frame.target.height = GPU_TARGET_HEIGHT * m_gpu_scale;
	frame.target.persistent = true;
	frame.target.depth = osd::gpu_depth_mode::NONE;
	frame.target.msaa_samples = m_gpu_msaa;
	frame.target.generation = m_gpu_generation;
	frame.vertices = m_gpu_vertices.data();
	frame.vertex_count = m_gpu_vertices.size();
	frame.varyings_per_vertex = MIDVUNIT_SHADER_VARYINGS;
	frame.prim_state = m_gpu_state.data();
	frame.prim_count = m_gpu_state.size() / MIDVUNIT_SHADER_STATE_WORDS;
	frame.state_stride = MIDVUNIT_SHADER_STATE_WORDS;
	frame.commands = m_gpu_commands.data();
	frame.command_count = m_gpu_commands.size();
	frame.readbacks = changed ? &readback : nullptr;
	frame.readback_count = changed ? 1 : 0;

	const bool submitted = m_gpu_engine->submit_frame(frame);
	m_gpu_commands.clear();
	m_gpu_vertices.clear();
	m_gpu_state.clear();
	m_gpu_blobs.clear();
	m_gpu_clip_page = -1;

	if (!submitted)
	{
		gpu_latch_off("the engine refused a frame");
		m_state.m_video_changed = true;
		return false;
	}
	return changed;
}
