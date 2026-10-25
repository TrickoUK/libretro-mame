// license:BSD-3-Clause
// copyright-holders:TrickoUK
/***************************************************************************

    Namco System 23 / Super System 23 host-GPU path (included by
    namcos23.cpp)

    The sorted polygon list that render_flush() would hand to the scanline
    routines is turned into one triangle list in the same order, with a state
    record per polygon that carries everything the scanline routine reads
    from the render data. The pixel pipeline is namcos23_shader.h. The
    picture the text layer has made so far is the first primitive, so blended
    polygons see it as their backdrop, and the finished picture comes back as
    ordinary pixels with a drawn marker in alpha.

***************************************************************************/

bool namcos23_renderer::gpu_init()
{
	if (!m_gpu_checked)
	{
		m_gpu_checked = true;

		// Gorgon draws sprites and the depth compare tables in the same sorted list
		if (dynamic_cast<const gorgon_state *>(&m_state))
			return false;

		m_gpu_engine = m_state.machine().osd().get_gpu_engine("namcos23");
		if (m_gpu_engine && m_gpu_engine->caps().max_texture_size >= 4096)
		{
			osd::gpu_shader_module module;
			module.name = "namcos23";
			module.fragment_source = NAMCOS23_FRAGMENT_SOURCE;
			module.blend_factor_output = true;
			if (m_state.machine().osd().msaa() > 0)
				module.centroid_mask = 0xf;
			module.varying_count = NAMCOS23_SHADER_VARYINGS;
			for (int i = 0; i < NAMCOS23_SHADER_VARYINGS; i++)
				module.varying_mode[i] = osd::gpu_varying_mode::NOPERSPECTIVE;
			module.state_stride = NAMCOS23_SHADER_STATE_WORDS;
			module.binding_count = 5;
			module.binding_name[0] = "tile_data";
			module.binding_format[0] = osd::gpu_texture_format::R32UI;
			module.binding_name[1] = "tile_map";
			module.binding_format[1] = osd::gpu_texture_format::R32UI;
			module.binding_name[2] = "palette";
			module.binding_format[2] = osd::gpu_texture_format::RGBA8;
			module.binding_name[3] = "stencil";
			module.binding_format[3] = osd::gpu_texture_format::R16UI;
			module.binding_name[4] = "picture";
			module.binding_format[4] = osd::gpu_texture_format::RGBA8;
			m_gpu_module = m_gpu_engine->create_shader_module(module);

			// the texture ROM is read as 32-bit words, four texels each
			osd::gpu_texture_desc desc;
			desc.format = osd::gpu_texture_format::R32UI;
			desc.width = GPU_TILE_DATA_WIDTH;
			desc.height = (m_tile_mask + 1) * 256 / (GPU_TILE_DATA_WIDTH * 4);
			m_gpu_tile_data = desc.height ? m_gpu_engine->create_texture(desc) : osd::GPU_NULL_HANDLE;
			desc.width = GPU_TILE_MAP_WIDTH;
			desc.height = ((m_tileid_mask | 0xff) + GPU_TILE_MAP_WIDTH) / GPU_TILE_MAP_WIDTH;
			m_gpu_tile_map = m_gpu_engine->create_texture(desc);
			desc.format = osd::gpu_texture_format::RGBA8;
			desc.width = 256;
			desc.height = 128;
			m_gpu_palette = m_gpu_engine->create_texture(desc);
			desc.format = osd::gpu_texture_format::R16UI;
			desc.width = GPU_STENCIL_WIDTH;
			desc.height = 0x20000 / GPU_STENCIL_WIDTH;
			m_gpu_stencil = m_gpu_engine->create_texture(desc);
			desc.format = osd::gpu_texture_format::RGBA8;
			desc.width = GPU_SCREEN_WIDTH;
			desc.height = GPU_SCREEN_HEIGHT;
			m_gpu_picture = m_gpu_engine->create_texture(desc);

			if (!m_gpu_module || !m_gpu_tile_data || !m_gpu_tile_map || !m_gpu_palette || !m_gpu_stencil || !m_gpu_picture)
			{
				m_gpu_engine = nullptr;
			}
			else
			{
				// the decoded tile table packs the flip and swap attributes above the tile's byte offset
				const u32 entries = (m_tileid_mask | 0xff) + 1;
				m_gpu_tile_map_data = std::make_unique<u32[]>(entries);
				for (u32 i = 0; i < entries; i++)
					m_gpu_tile_map_data[i] = m_tmrom_decoded[i] | (u32(m_texattr_decoded[i] & 7) << 28);
				m_gpu_shadow_palette = make_unique_clear<u32[]>(0x8000);
				m_gpu_shadow_stencil = make_unique_clear<u16[]>(0x20000);
			}
		}
		else
		{
			if (m_gpu_engine)
				osd_printf_verbose("namcos23: host GPU path unavailable, the largest texture is %d texels and 4096 are needed\n", m_gpu_engine->caps().max_texture_size);
			m_gpu_engine = nullptr;
		}
	}
	return m_gpu_engine != nullptr && !m_gpu_engine->failed();
}

// Queues the rows of a texture that differ from its shadow copy (all of them when forced) as one update, and
// refreshes the shadow.
template <typename T>
void namcos23_renderer::gpu_update_changed(osd::gpu_texture_update *updates, int &update_count, osd::gpu_handle texture, T *shadow, const T *source, int entries, int width, bool force)
{
	const int rows = entries / width;
	int first = -1;
	int last = -1;
	for (int row = 0; row < rows; row++)
	{
		if (force || memcmp(shadow + row * width, source + row * width, width * sizeof(T)))
		{
			if (first < 0)
				first = row;
			last = row;
		}
	}
	if (first < 0)
		return;

	memcpy(shadow + first * width, source + first * width, (last - first + 1) * width * sizeof(T));
	osd::gpu_texture_update &update = updates[update_count++];
	update.texture = texture;
	update.y = first;
	update.w = width;
	update.h = last - first + 1;
	update.data = shadow + first * width;
}

void namcos23_renderer::gpu_add_poly(const namcos23_poly_entry &p)
{
	struct clip_vertex
	{
		double x, y;
		double p[4];
	};

	const namcos23_render_data &rd = p.rd;
	u32 d[NAMCOS23_SHADER_STATE_WORDS] = {};
	int penmask = 0xff;
	int penshift = 0;
	u32 penbase = rd.pens - &m_state.m_palette->pen(0);

	if (rd.cmode & 4)
	{
		penbase += 0xec + ((rd.cmode & 8) << 1);
		penmask = 0x03;
		penshift = 2 * (~rd.cmode & 3);
	}
	else if (rd.cmode & 2)
	{
		penbase += 0xe0 + ((rd.cmode & 8) << 1);
		penmask = 0x0f;
		penshift = 4 * (~rd.cmode & 1);
	}

	// the polygon covers its viewport on the screen
	const int clip_left = (320 - rd.vp_size_x) + rd.vp_offset_x;
	const int clip_right = (320 + rd.vp_size_x) + rd.vp_offset_x;
	const int clip_top = (240 - rd.vp_size_y) - rd.vp_offset_y;
	const int clip_bottom = (240 + rd.vp_size_y) - rd.vp_offset_y;
	const int min_x = std::max(clip_left, 0);
	const int max_x = std::min(clip_right, 640);
	const int min_y = std::max(clip_top, 0);
	const int max_y = std::min(clip_bottom, 480);
	if (min_x >= max_x || min_y >= max_y)
		return;

	const float intensity_base = rd.intensity_base;
	d[0] = (rd.stencil_enabled ? 1 : 0) | (rd.shade_enabled ? 2 : 0) | (rd.pfade_enabled ? 4 : 0) | ((rd.fadefactor != 0xff) ? 8 : 0)
			| (rd.blend_enabled ? 16 : 0) | ((rd.alpha != 0xff) ? 32 : 0) | (rd.alpha_enabled ? 64 : 0)
			| (m_state.machine().osd().texture_filter() ? 256 : 0);
	d[1] = penbase;
	d[2] = penmask;
	d[3] = penshift;
	d[4] = rd.tbase;
	d[5] = m_tileid_mask;
	memcpy(&d[6], &intensity_base, sizeof(d[6]));
	d[7] = rd.polycolor_r;
	d[8] = rd.polycolor_g;
	d[9] = rd.polycolor_b;
	d[10] = rd.fadefactor;
	d[11] = rd.fadecolor_r;
	d[12] = rd.fadecolor_g;
	d[13] = rd.fadecolor_b;
	d[14] = rd.alpha;
	d[15] = rd.poly_alpha_pen;
	d[16] = min_x * m_gpu_scale;
	d[17] = min_y * m_gpu_scale;
	d[18] = max_x * m_gpu_scale;
	d[19] = max_y * m_gpu_scale;

	// The vertices are relative to the viewport. The varyings are screen-linear, so clipping a triangle to the
	// polygon's rectangle (plus a one pixel margin) in double precision is exact, and keeps near-plane vertices that
	// lie far outside the screen from losing precision in the GPU interpolation. Each triangle of the fan is clipped on
	// its own, because the varyings of a polygon that is not planar depend on how it is split.
	clip_vertex fan[16];
	const int count = p.vertex_count;
	for (int i = 0; i < count; i++)
	{
		fan[i].x = double(p.pv[i].x) + clip_left;
		fan[i].y = double(p.pv[i].y) + clip_top;
		for (int j = 0; j < 4; j++)
			fan[i].p[j] = p.pv[i].p[j];
	}
	const double limits[4] = { double(min_x) - 1.0, double(max_x) + 1.0, double(min_y) - 1.0, double(max_y) + 1.0 };

	u32 prim = ~u32(0);
	for (int tri = 1; tri + 1 < count; tri++)
	{
		clip_vertex poly[2][12];
		poly[0][0] = fan[0];
		poly[0][1] = fan[tri];
		poly[0][2] = fan[tri + 1];
		int n = 3;
		int cur = 0;
		for (int edge = 0; edge < 4 && n >= 3; edge++)
		{
			const bool horizontal = edge < 2;
			const bool upper = (edge & 1) != 0;
			const double limit = limits[edge];
			auto inside = [&] (const clip_vertex &v)
			{
				const double c = horizontal ? v.x : v.y;
				return upper ? (c <= limit) : (c >= limit);
			};
			clip_vertex *const in = poly[cur];
			clip_vertex *const out = poly[cur ^ 1];
			int out_count = 0;
			for (int i = 0; i < n; i++)
			{
				const clip_vertex &a = in[i];
				const clip_vertex &b = in[(i + 1) % n];
				const bool a_in = inside(a);
				const bool b_in = inside(b);
				if (a_in)
					out[out_count++] = a;
				if (a_in != b_in)
				{
					const double ca = horizontal ? a.x : a.y;
					const double cb = horizontal ? b.x : b.y;
					const double t = (limit - ca) / (cb - ca);
					clip_vertex &v = out[out_count++];
					v.x = a.x + (b.x - a.x) * t;
					v.y = a.y + (b.y - a.y) * t;
					for (int j = 0; j < 4; j++)
						v.p[j] = a.p[j] + (b.p[j] - a.p[j]) * t;
					if (horizontal)
						v.x = limit;
					else
						v.y = limit;
				}
			}
			n = out_count;
			cur ^= 1;
		}
		if (n < 3)
			continue;

		if (prim == ~u32(0))
		{
			prim = m_gpu_state.size() / NAMCOS23_SHADER_STATE_WORDS;
			m_gpu_state.insert(m_gpu_state.end(), d, d + NAMCOS23_SHADER_STATE_WORDS);
		}
		gpu_vertex vv[12];
		for (int i = 0; i < n; i++)
		{
			vv[i] = { float(poly[cur][i].x * m_gpu_scale), float(poly[cur][i].y * m_gpu_scale), 0.0f, 1.0f, prim, {} };
			for (int j = 0; j < 4; j++)
				vv[i].v[j] = float(poly[cur][i].p[j]);
		}
		for (int i = 1; i + 1 < n; i++)
		{
			m_gpu_vertices.push_back(vv[0]);
			m_gpu_vertices.push_back(vv[i]);
			m_gpu_vertices.push_back(vv[i + 1]);
		}
	}
}

// Draws the sorted polygon list on the host GPU into bitmap, which holds the background and the text layer under the
// 3D and is replaced by the picture the polygons leave; the covered pixels are marked in the screen's priority map.
// Returns false, leaving the list and the bitmap untouched, when the GPU cannot produce the frame; the caller then
// renders it in software.
bool namcos23_renderer::gpu_render(screen_device &screen, bitmap_rgb32 &bitmap)
{
	if (!gpu_init())
		return false;
	if (bitmap.width() < GPU_SCREEN_WIDTH || bitmap.height() < GPU_SCREEN_HEIGHT)
		return false;

	render_t &render = m_state.m_render;
	for (int i = 0; i < render.poly_count; i++)
		if (render.poly_order[i]->rd.sprite)
			return false;

	// supersampling: the target is scale times the bitmap in each direction, averaged down on the way back
	const int max_size = m_gpu_engine->caps().max_texture_size;
	m_gpu_scale = std::clamp(m_state.machine().osd().supersample(), 1, 4);
	while (m_gpu_scale > 1 && (GPU_SCREEN_WIDTH * m_gpu_scale > max_size || GPU_SCREEN_HEIGHT * m_gpu_scale > max_size))
		m_gpu_scale--;

	m_gpu_state.clear();
	m_gpu_vertices.clear();

	// Polygons blend with the picture the text layer already made, so it is the first primitive.
	{
		u32 d[NAMCOS23_SHADER_STATE_WORDS] = {};
		d[0] = 128;
		d[18] = GPU_SCREEN_WIDTH * m_gpu_scale;
		d[19] = GPU_SCREEN_HEIGHT * m_gpu_scale;
		d[20] = m_gpu_scale;
		m_gpu_state.insert(m_gpu_state.end(), d, d + NAMCOS23_SHADER_STATE_WORDS);
		const float w = GPU_SCREEN_WIDTH * m_gpu_scale;
		const float h = GPU_SCREEN_HEIGHT * m_gpu_scale;
		const gpu_vertex corners[4] = { { 0, 0, 0, 1, 0, {} }, { w, 0, 0, 1, 0, {} }, { w, h, 0, 1, 0, {} }, { 0, h, 0, 1, 0, {} } };
		for (int i : { 0, 1, 2, 0, 2, 3 })
			m_gpu_vertices.push_back(corners[i]);
	}
	const u32 stamp_vertices = m_gpu_vertices.size();

	for (int i = 0; i < render.poly_count; i++)
	{
		const namcos23_poly_entry &p = *render.poly_order[i];
		if (p.vertex_count >= 3 && p.vertex_count <= 6)
			gpu_add_poly(p);
	}

	const u32 prim_count = m_gpu_state.size() / NAMCOS23_SHADER_STATE_WORDS;
	if (m_gpu_vertices.size() > stamp_vertices)
	{
		// the ROM tables are fixed after video_start(); the palette and the stencil RAM change while running
		osd::gpu_texture_update updates[6];
		int update_count = 0;
		if (m_gpu_full_upload)
		{
			osd::gpu_texture_update &data = updates[update_count++];
			data.texture = m_gpu_tile_data;
			data.w = GPU_TILE_DATA_WIDTH;
			data.h = (m_tile_mask + 1) * 256 / (GPU_TILE_DATA_WIDTH * 4);
			data.data = m_texrom;

			osd::gpu_texture_update &map = updates[update_count++];
			map.texture = m_gpu_tile_map;
			map.w = GPU_TILE_MAP_WIDTH;
			map.h = ((m_tileid_mask | 0xff) + 1) / GPU_TILE_MAP_WIDTH;
			map.data = m_gpu_tile_map_data.get();
		}
		gpu_update_changed(updates, update_count, m_gpu_palette, m_gpu_shadow_palette.get(), &m_state.m_palette->pen(0), 0x8000, 256, m_gpu_full_upload);
		gpu_update_changed(updates, update_count, m_gpu_stencil, m_gpu_shadow_stencil.get(), m_texram, 0x20000, GPU_STENCIL_WIDTH, m_gpu_full_upload);
		m_gpu_full_upload = false;

		osd::gpu_texture_update &picture = updates[update_count++];
		picture.texture = m_gpu_picture;
		picture.w = GPU_SCREEN_WIDTH;
		picture.h = GPU_SCREEN_HEIGHT;
		picture.data = &bitmap.pix(0);
		picture.pitch_bytes = bitmap.rowpixels() * sizeof(u32);

		// Polygons are multisampled; the stamp is drawn at the pixel centre into every sample. The colour blends by
		// the shader's weight, the alpha channel (the drawn marker) takes the new value.
		osd::gpu_command draw = osd::gpu_command::make_draw(m_gpu_module, 0, 0);
		draw.variant = osd::gpu_aniso_variant(m_state.machine().osd().anisotropy());
		draw.bindings[0] = m_gpu_tile_data;
		draw.bindings[1] = m_gpu_tile_map;
		draw.bindings[2] = m_gpu_palette;
		draw.bindings[3] = m_gpu_stencil;
		draw.bindings[4] = m_gpu_picture;
		draw.blend.enable = true;
		draw.blend.src = osd::gpu_blend_factor::SRC1_ALPHA;
		draw.blend.dst = osd::gpu_blend_factor::ONE_MINUS_SRC1_ALPHA;
		draw.blend.separate_alpha = true;
		draw.blend.src_alpha = osd::gpu_blend_factor::ONE;
		draw.blend.dst_alpha = osd::gpu_blend_factor::ZERO;
		osd::gpu_command draws[2] = { draw, draw };
		draws[0].first_vertex = 0;
		draws[0].vertex_count = stamp_vertices;
		draws[0].multisample = false;
		draws[1].first_vertex = stamp_vertices;
		draws[1].vertex_count = m_gpu_vertices.size() - stamp_vertices;

		osd::gpu_readback_rect readback;
		readback.w = GPU_SCREEN_WIDTH * m_gpu_scale;
		readback.h = GPU_SCREEN_HEIGHT * m_gpu_scale;
		readback.out = &bitmap.pix(0);
		readback.stride = bitmap.rowpixels();
		const int msaa = m_state.machine().osd().msaa();
		if (m_gpu_scale > 1 || msaa > 0)
		{
			readback.downsample = m_gpu_scale;
			readback.priority_bits = true;
		}

		osd::gpu_frame frame;
		frame.target.width = GPU_SCREEN_WIDTH * m_gpu_scale;
		frame.target.height = GPU_SCREEN_HEIGHT * m_gpu_scale;
		frame.target.depth = osd::gpu_depth_mode::NONE;
		frame.target.msaa_samples = msaa;
		frame.target.msaa_per_sample = msaa > 0;
		frame.updates = updates;
		frame.update_count = update_count;
		frame.vertices = m_gpu_vertices.data();
		frame.vertex_count = m_gpu_vertices.size();
		frame.varyings_per_vertex = NAMCOS23_SHADER_VARYINGS;
		frame.prim_state = m_gpu_state.data();
		frame.prim_count = prim_count;
		frame.state_stride = NAMCOS23_SHADER_STATE_WORDS;
		frame.commands = draws;
		frame.command_count = 2;
		frame.readbacks = &readback;
		frame.readback_count = 1;
		if (!m_gpu_engine->submit_frame(frame))
		{
			// the tables may not have reached the GPU
			m_gpu_full_upload = true;
			return false;
		}

		// The alpha of a covered pixel is non-zero; the text layer is mixed again over those pixels. The bitmap is left
		// opaque like the picture the scanline routines make.
		for (int y = 0; y < GPU_SCREEN_HEIGHT; y++)
		{
			u32 *const dest = &bitmap.pix(y);
			u8 *const primap = &screen.priority().pix(y);
			for (int x = 0; x < GPU_SCREEN_WIDTH; x++)
			{
				if (dest[x] >> 24)
					primap[x] = (primap[x] & ~1) | 2;
				dest[x] |= 0xff000000;
			}
		}
	}

	render.poly_count = 0;
	return true;
}
