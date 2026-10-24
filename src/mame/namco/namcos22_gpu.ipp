// license:BSD-3-Clause
// copyright-holders:TrickoUK
/***************************************************************************

    Namco System 22 / Super System 22 host-GPU path (included by
    namcos22_v.cpp)

    The scene nodes (quads and scaled sprites, already sorted far to near)
    are turned into one triangle list in the same order, with a state record
    per primitive that carries everything the scanline routines read from
    the object data. The pixel pipeline is namcos22_shader.h. The finished
    picture comes back as ordinary pixels with the priority bits in alpha.

***************************************************************************/

bool namcos22_renderer::gpu_init()
{
	if (!m_gpu_checked)
	{
		m_gpu_checked = true;
		m_gpu_engine = m_state.machine().osd().get_gpu_engine("namcos22");
		if (m_gpu_engine && m_gpu_engine->caps().max_texture_size >= 4096)
		{
			osd::gpu_shader_module module;
			module.name = "namcos22";
			module.fragment_source = NAMCOS22_FRAGMENT_SOURCE;
			module.blend_factor_output = true;
			module.varying_count = NAMCOS22_SHADER_VARYINGS;
			for (int i = 0; i < NAMCOS22_SHADER_VARYINGS; i++)
				module.varying_mode[i] = osd::gpu_varying_mode::NOPERSPECTIVE;
			module.state_stride = NAMCOS22_SHADER_STATE_WORDS;
			module.binding_count = 6;
			module.binding_name[0] = "tile_data";
			module.binding_format[0] = osd::gpu_texture_format::R8UI;
			module.binding_name[1] = "tile_map";
			module.binding_format[1] = osd::gpu_texture_format::R16UI;
			module.binding_name[2] = "tile_attr";
			module.binding_format[2] = osd::gpu_texture_format::R8UI;
			module.binding_name[3] = "sprite_data";
			module.binding_format[3] = osd::gpu_texture_format::R8UI;
			module.binding_name[4] = "palette";
			module.binding_format[4] = osd::gpu_texture_format::RGBA8;
			module.binding_name[5] = "picture";
			module.binding_format[5] = osd::gpu_texture_format::RGBA8;
			m_gpu_module = m_gpu_engine->create_shader_module(module);

			osd::gpu_texture_desc desc;
			desc.format = osd::gpu_texture_format::R8UI;
			desc.width = 4096;
			desc.height = 4096;
			m_gpu_tile_data = m_gpu_engine->create_texture(desc);
			desc.format = osd::gpu_texture_format::R16UI;
			desc.width = 1024;
			desc.height = 1024;
			m_gpu_tile_map = m_gpu_engine->create_texture(desc);
			desc.format = osd::gpu_texture_format::R8UI;
			m_gpu_tile_attr = m_gpu_engine->create_texture(desc);
			desc.width = 4096;
			desc.height = m_state.m_is_ss22 ? (m_state.m_gfxdecode->gfx(2)->elements() * 1024) / 4096 : 1;
			m_gpu_sprite_data = m_gpu_engine->create_texture(desc);
			desc.format = osd::gpu_texture_format::RGBA8;
			desc.width = 256;
			desc.height = 128;
			m_gpu_palette = m_gpu_engine->create_texture(desc);
			// Super System 22 creates its picture texture at the screen size on the first frame.
			desc.width = 1;
			desc.height = 1;
			if (!m_state.m_is_ss22)
				m_gpu_picture = m_gpu_engine->create_texture(desc);

			if (!m_gpu_module || !m_gpu_tile_data || !m_gpu_tile_map || !m_gpu_tile_attr || !m_gpu_sprite_data || !m_gpu_palette || (!m_state.m_is_ss22 && !m_gpu_picture))
				m_gpu_engine = nullptr;
			else
				m_gpu_shadow_palette = make_unique_clear<u32[]>(0x8000);
		}
		else
			m_gpu_engine = nullptr;
	}
	return m_gpu_engine != nullptr && !m_gpu_engine->failed();
}

u32 namcos22_renderer::gpu_pen_index(const pen_t *pens) const
{
	return pens - &m_state.m_palette->pen(0);
}

void namcos22_renderer::gpu_add_vertices(const vertex_t *verts, int count, int varyings, u32 prim)
{
	gpu_vertex vv[12];
	for (int i = 0; i < count; i++)
	{
		vv[i] = { float(verts[i].x), float(verts[i].y), 0.0f, 1.0f, prim, {} };
		for (int j = 0; j < varyings; j++)
			vv[i].v[j] = float(verts[i].p[j]);
	}
	for (int i = 1; i + 1 < count; i++)
	{
		m_gpu_vertices.push_back(vv[0]);
		m_gpu_vertices.push_back(vv[i]);
		m_gpu_vertices.push_back(vv[i + 1]);
	}
}

// Appends one quad (3 to 6 vertices after the near-plane clip) to the GPU frame.
void namcos22_renderer::gpu_add_quad(const namcos22_object_data &extra, int clipverts, vertex_t const *clipv)
{
	u32 d[NAMCOS22_SHADER_STATE_WORDS] = {};
	int penmask = 0xff;
	int penshift = 0;
	u32 penbase = gpu_pen_index(extra.pens);

	if (extra.cmode & 4)
	{
		penbase += 0xec + ((extra.cmode & 8) << 1);
		penmask = 0x03;
		penshift = 2 * (~extra.cmode & 3);
	}
	else if (extra.cmode & 2)
	{
		penbase += 0xe0 + ((extra.cmode & 8) << 1);
		penmask = 0x0f;
		penshift = 4 * (~extra.cmode & 1);
	}

	d[0] = (extra.texture_enabled ? 2 : 0) | (extra.shade_enabled ? 4 : 0) | (m_state.m_is_ss22 ? 8 : 0)
			| (extra.pfade_enabled ? 16 : 0) | (extra.alpha_enabled ? 32 : 0);
	d[1] = penbase;
	d[2] = penmask;
	d[3] = penshift;
	d[4] = extra.bn * 0x1000;
	d[5] = 0xff - extra.fogfactor;
	d[6] = extra.fogcolor.get_r();
	d[7] = extra.fogcolor.get_g();
	d[8] = extra.fogcolor.get_b();
	d[9] = 0xff - extra.fadefactor;
	d[10] = extra.fadecolor.get_r();
	d[11] = extra.fadecolor.get_g();
	d[12] = extra.fadecolor.get_b();
	d[13] = extra.polycolor.get_r();
	d[14] = extra.polycolor.get_g();
	d[15] = extra.polycolor.get_b();
	d[16] = 0xff - extra.alpha;
	d[17] = m_state.m_poly_alpha_pen;
	d[18] = m_cliprect.min_x;
	d[19] = m_cliprect.min_y;
	d[20] = m_cliprect.max_x + 1;
	d[21] = m_cliprect.max_y + 1;
	d[25] = extra.prioverchar & 1;

	const u32 prim = m_gpu_state.size() / NAMCOS22_SHADER_STATE_WORDS;
	m_gpu_state.insert(m_gpu_state.end(), d, d + NAMCOS22_SHADER_STATE_WORDS);

	// The varyings are screen-linear, so clipping the polygon to the clip rectangle (plus a one pixel margin) in
	// double precision is exact, and keeps near-plane vertices that lie far outside the screen from losing
	// precision in the GPU interpolation.
	vertex_t poly[2][12];
	int count = clipverts;
	std::copy_n(clipv, clipverts, poly[0]);
	const double limits[4] = { double(m_cliprect.min_x) - 1.0, double(m_cliprect.max_x) + 2.0, double(m_cliprect.min_y) - 1.0, double(m_cliprect.max_y) + 2.0 };
	int cur = 0;
	for (int edge = 0; edge < 4 && count >= 3; edge++)
	{
		const bool horizontal = edge < 2;
		const bool upper = (edge & 1) != 0;
		const double limit = limits[edge];
		auto inside = [&] (const vertex_t &v)
		{
			const double c = horizontal ? v.x : v.y;
			return upper ? (c <= limit) : (c >= limit);
		};
		vertex_t *const in = poly[cur];
		vertex_t *const out = poly[cur ^ 1];
		int out_count = 0;
		for (int i = 0; i < count; i++)
		{
			const vertex_t &a = in[i];
			const vertex_t &b = in[(i + 1) % count];
			const bool a_in = inside(a);
			const bool b_in = inside(b);
			if (a_in)
				out[out_count++] = a;
			if (a_in != b_in)
			{
				const double ca = horizontal ? a.x : a.y;
				const double cb = horizontal ? b.x : b.y;
				const double t = (limit - ca) / (cb - ca);
				vertex_t &n = out[out_count++];
				n.x = a.x + (b.x - a.x) * t;
				n.y = a.y + (b.y - a.y) * t;
				for (int j = 0; j < 4; j++)
					n.p[j] = a.p[j] + (b.p[j] - a.p[j]) * t;
				if (horizontal)
					n.x = limit;
				else
					n.y = limit;
			}
		}
		count = out_count;
		cur ^= 1;
	}
	if (count >= 3)
		gpu_add_vertices(poly[cur], count, 4, prim);
}

// Appends one scaled sprite (Super System 22) to the GPU frame.
void namcos22_renderer::gpu_add_sprite(const namcos22_object_data &extra, vertex_t const *vert)
{
	u32 d[NAMCOS22_SHADER_STATE_WORDS] = {};
	gfx_element *const gfx = m_state.m_gfxdecode->gfx(2);

	d[0] = 1 | 8 | (extra.alpha_enabled ? 32 : 0);
	d[1] = gpu_pen_index(extra.pens);
	d[5] = 0xff - extra.fogfactor;
	d[6] = extra.fogcolor.get_r();
	d[7] = extra.fogcolor.get_g();
	d[8] = extra.fogcolor.get_b();
	d[9] = 0xff - extra.fadefactor;
	d[10] = extra.fadecolor.get_r();
	d[11] = extra.fadecolor.get_g();
	d[12] = extra.fadecolor.get_b();
	d[16] = extra.alpha;
	d[17] = m_state.m_poly_alpha_pen;
	d[18] = m_cliprect.min_x;
	d[19] = m_cliprect.min_y;
	d[20] = m_cliprect.max_x + 1;
	d[21] = m_cliprect.max_y + 1;
	d[22] = extra.source - gfx->get_data(0);
	d[23] = extra.line_modulo;
	d[24] = (extra.flipx ? 1 : 0) | (extra.flipy ? 2 : 0);
	d[25] = extra.prioverchar & 1;

	const u32 prim = m_gpu_state.size() / NAMCOS22_SHADER_STATE_WORDS;
	m_gpu_state.insert(m_gpu_state.end(), d, d + NAMCOS22_SHADER_STATE_WORDS);
	gpu_add_vertices(vert, 4, 2, prim);
}

// Turns the scene nodes into GPU primitives without consuming them, far to near like render_scene_nodes().
void namcos22_renderer::gpu_collect_nodes(screen_device &screen, bitmap_rgb32 &bitmap, struct namcos22_scenenode *node)
{
	if (!node)
		return;

	if (node->type == NAMCOS22_SCENENODE_NONLEAF)
	{
		for (int i = NAMCOS22_RADIX_BUCKETS - 1; i >= 0; i--)
			gpu_collect_nodes(screen, bitmap, node->data.nonleaf.next[i]);
	}
	else
	{
		for ( ; node; node = node->next)
		{
			if (node->type == NAMCOS22_SCENENODE_QUAD)
				poly3d_drawquad(screen, bitmap, node);
			else
				render_sprite(screen, bitmap, node);
		}
	}
}

// Frees the nodes the way render_scene_nodes() does once they are drawn.
void namcos22_renderer::gpu_release_nodes(struct namcos22_scenenode *node)
{
	if (!node)
		return;

	if (node->type == NAMCOS22_SCENENODE_NONLEAF)
	{
		for (int i = NAMCOS22_RADIX_BUCKETS - 1; i >= 0; i--)
			gpu_release_nodes(node->data.nonleaf.next[i]);
		free_scenenode(node);
	}
	else
	{
		while (node)
		{
			struct namcos22_scenenode *next = node->next;
			free_scenenode(node);
			node = next;
		}
	}
}

// Draws the scene on the host GPU into bitmap, which holds the background and is replaced by the picture the
// polygons leave; the priority bits of the covered pixels go to the screen's priority map. Returns false, leaving
// the scene untouched, when the GPU cannot produce the frame; the caller then renders it in software.
bool namcos22_renderer::gpu_render_scene(screen_device &screen, bitmap_rgb32 &bitmap, const rectangle &cliprect, rgb_t background, bool stamp)
{
	if (!gpu_init())
		return false;
	if (stamp && !m_gpu_picture)
	{
		osd::gpu_texture_desc desc;
		desc.format = osd::gpu_texture_format::RGBA8;
		desc.width = bitmap.width();
		desc.height = bitmap.height();
		m_gpu_picture = m_gpu_engine->create_texture(desc);
		m_gpu_picture_size = { desc.width, desc.height };
	}
	if (stamp && (!m_gpu_picture || bitmap.width() != m_gpu_picture_size.first || bitmap.height() != m_gpu_picture_size.second))
		return false;

	m_gpu_state.clear();
	m_gpu_vertices.clear();

	// Polygons and sprites blend with the picture the text layer already made, so it is the first primitive.
	if (stamp)
	{
		u32 d[NAMCOS22_SHADER_STATE_WORDS] = {};
		d[0] = 64;
		d[20] = bitmap.width();
		d[21] = bitmap.height();
		m_gpu_state.insert(m_gpu_state.end(), d, d + NAMCOS22_SHADER_STATE_WORDS);
		const float w = bitmap.width();
		const float h = bitmap.height();
		const gpu_vertex corners[4] = { { 0, 0, 0, 1, 0, {} }, { w, 0, 0, 1, 0, {} }, { w, h, 0, 1, 0, {} }, { 0, h, 0, 1, 0, {} } };
		for (int i : { 0, 1, 2, 0, 2, 3 })
			m_gpu_vertices.push_back(corners[i]);
	}
	const u32 stamp_prims = m_gpu_state.size() / NAMCOS22_SHADER_STATE_WORDS;

	m_gpu_collecting = true;
	for (int i = NAMCOS22_RADIX_BUCKETS - 1; i >= 0; i--)
		gpu_collect_nodes(screen, bitmap, m_scenenode_root.data.nonleaf.next[i]);
	m_gpu_collecting = false;

	const u32 prim_count = m_gpu_state.size() / NAMCOS22_SHADER_STATE_WORDS;
	if (prim_count > stamp_prims)
	{
		// The tile and sprite tables are fixed after video_start(); only the palette changes while running.
		osd::gpu_texture_update updates[6];
		int update_count = 0;
		if (m_gpu_full_upload)
		{
			osd::gpu_texture_update &data = updates[update_count++];
			data.texture = m_gpu_tile_data;
			data.w = 4096;
			data.h = 4096;
			data.data = m_state.m_texture_tiledata;

			osd::gpu_texture_update &map = updates[update_count++];
			map.texture = m_gpu_tile_map;
			map.w = 1024;
			map.h = 1024;
			map.data = m_state.m_texture_tilemap;

			osd::gpu_texture_update &attr = updates[update_count++];
			attr.texture = m_gpu_tile_attr;
			attr.w = 1024;
			attr.h = 1024;
			attr.data = m_state.m_texture_tileattr.get();

			if (m_state.m_is_ss22)
			{
				gfx_element *const gfx = m_state.m_gfxdecode->gfx(2);
				gfx->decode_all();
				osd::gpu_texture_update &sprite = updates[update_count++];
				sprite.texture = m_gpu_sprite_data;
				sprite.w = 4096;
				sprite.h = (gfx->elements() * 1024) / 4096;
				sprite.data = gfx->get_data(0);
			}
		}
		const pen_t *const pens = &m_state.m_palette->pen(0);
		if (m_gpu_full_upload || memcmp(m_gpu_shadow_palette.get(), pens, 0x8000 * sizeof(u32)))
		{
			memcpy(m_gpu_shadow_palette.get(), pens, 0x8000 * sizeof(u32));
			osd::gpu_texture_update &pal = updates[update_count++];
			pal.texture = m_gpu_palette;
			pal.w = 256;
			pal.h = 128;
			pal.data = m_gpu_shadow_palette.get();
		}
		m_gpu_full_upload = false;
		if (stamp)
		{
			osd::gpu_texture_update &picture = updates[update_count++];
			picture.texture = m_gpu_picture;
			picture.w = bitmap.width();
			picture.h = bitmap.height();
			picture.data = &bitmap.pix(0);
			picture.pitch_bytes = bitmap.rowpixels() * sizeof(u32);
		}

		osd::gpu_command draw = osd::gpu_command::make_draw(m_gpu_module, 0, m_gpu_vertices.size());
		draw.bindings[0] = m_gpu_tile_data;
		draw.bindings[1] = m_gpu_tile_map;
		draw.bindings[2] = m_gpu_tile_attr;
		draw.bindings[3] = m_gpu_sprite_data;
		draw.bindings[4] = m_gpu_palette;
		draw.bindings[5] = m_gpu_picture;
		if (m_state.m_is_ss22)
		{
			// the colour blends by the shader's weight, the alpha channel (priority bits) takes the new value
			draw.blend.enable = true;
			draw.blend.src = osd::gpu_blend_factor::SRC1_ALPHA;
			draw.blend.dst = osd::gpu_blend_factor::ONE_MINUS_SRC1_ALPHA;
			draw.blend.separate_alpha = true;
			draw.blend.src_alpha = osd::gpu_blend_factor::ONE;
			draw.blend.dst_alpha = osd::gpu_blend_factor::ZERO;
		}

		osd::gpu_readback_rect readback;
		readback.x = cliprect.min_x;
		readback.y = cliprect.min_y;
		readback.w = cliprect.width();
		readback.h = cliprect.height();
		readback.out = &bitmap.pix(cliprect.min_y, cliprect.min_x);
		readback.stride = bitmap.rowpixels();

		osd::gpu_frame frame;
		frame.target.width = bitmap.width();
		frame.target.height = bitmap.height();
		frame.target.depth = osd::gpu_depth_mode::NONE;
		frame.target.clear_color[0] = background.r() / 255.0f;
		frame.target.clear_color[1] = background.g() / 255.0f;
		frame.target.clear_color[2] = background.b() / 255.0f;
		frame.target.clear_color[3] = 0.0f;
		frame.updates = updates;
		frame.update_count = update_count;
		frame.vertices = m_gpu_vertices.data();
		frame.vertex_count = m_gpu_vertices.size();
		frame.varyings_per_vertex = NAMCOS22_SHADER_VARYINGS;
		frame.prim_state = m_gpu_state.data();
		frame.prim_count = prim_count;
		frame.state_stride = NAMCOS22_SHADER_STATE_WORDS;
		frame.commands = &draw;
		frame.command_count = 1;
		frame.readbacks = &readback;
		frame.readback_count = 1;
		if (!m_gpu_engine->submit_frame(frame))
		{
			// the tables may not have reached the GPU
			m_gpu_full_upload = true;
			return false;
		}

		// The alpha of a covered pixel holds its priority bits: 128 = drawn, 255 = drawn over the text layer.
		const int drawn_bits = m_state.m_is_ss22 ? 2 : 0;
		for (int y = cliprect.min_y; y <= cliprect.max_y; y++)
		{
			u32 *const dest = &bitmap.pix(y);
			u8 *const primap = &screen.priority().pix(y);
			for (int x = cliprect.min_x; x <= cliprect.max_x; x++)
			{
				const u32 alpha = dest[x] >> 24;
				if (alpha != 0)
					primap[x] = (primap[x] & ~1) | drawn_bits | (alpha == 0xff ? 1 : 0);
				dest[x] &= 0x00ffffff;
			}
		}
	}

	for (int i = NAMCOS22_RADIX_BUCKETS - 1; i >= 0; i--)
	{
		gpu_release_nodes(m_scenenode_root.data.nonleaf.next[i]);
		m_scenenode_root.data.nonleaf.next[i] = nullptr;
	}
	return true;
}
