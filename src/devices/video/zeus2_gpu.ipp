// license:BSD-3-Clause
// copyright-holders:TrickoUK
/***************************************************************************

    Midway Zeus 2 host-GPU path (included by zeus2.cpp)

    Each polygon that render_poly() would rasterise becomes a fan of
    triangles in one command list, in the order the chip drew them, with a
    state record for its colour, texture and depth rules. The frame buffer is
    one persistent target, 512 pixels wide and 1024 rows tall: a polygon is
    drawn at the window its render registers name, and the display base picks
    the rows read back when the screen is updated.

    Wave RAM 0 is mirrored as one texture of 2048 by 2048 words. Each row of
    it that the chip writes is copied into the command list at the point of
    the write (the copy is taken when the next polygon is added, so a polygon
    samples the words as they stood when the chip drew it). The palette table
    is mirrored the same way, as one 256-entry texture.

    The frame buffer words the CPU writes (fast clears, fills and the direct
    writes of the FIFO) are drawn as solid quads at their point in the list,
    with the colour and depth the chip wrote. A CPU read of the frame buffer,
    a frame buffer that is not on the target rows, and a change to the
    frame buffer's shape (the second line mode) latch the path off for the
    rest of the session.

    With mame_3d_supersample the target holds a block of samples for each
    native pixel, and the read-back averages each block back to one pixel.
    Every coordinate the target is addressed by is multiplied by the scale.
    With mame_3d_msaa the target keeps coverage samples per pixel, resolved
    by the same read-back.

***************************************************************************/

namespace {

// state record words, as the shader reads them (zeus2_shader.h)
enum : uint32_t
{
	SW_KIND = 0,
	SW_COLOR,
	SW_TEXBASE,
	SW_TEXWIDTH,
	SW_MODE,
	SW_TRANSKEY,
	SW_DEPTH_CLEAR,
	SW_ZMIN,
	SW_BLEND,
	SW_SRCA,
	SW_COUNT
};

enum : uint32_t
{
	GPU_KIND_TEX = 0,
	GPU_KIND_SOLID,
	GPU_KIND_FILL
};

enum : uint32_t
{
	GPU_MODE_4BIT = 0,
	GPU_MODE_8BIT_4X2,
	GPU_MODE_8BIT_2X2,
	GPU_MODE_ALPHA,
	GPU_MODE_RGB555
};

static_assert(SW_COUNT == ZEUS2_SHADER_STATE_WORDS, "state record size");

// the target: 512 pixels per row, 1024 rows (the frame buffer's words from 0 to 512K)
constexpr int GPU_TARGET_WIDTH = 512;
constexpr int GPU_TARGET_ROWS = 1024;
constexpr uint32_t GPU_TARGET_WORDS = GPU_TARGET_WIDTH * GPU_TARGET_ROWS;

// wave RAM 0 as words: 2048 per texture row, 4M words (16 MB) in all
constexpr int GPU_TEXTURE_WIDTH = 2048;
constexpr int GPU_TEXTURE_ROWS = 2048;
constexpr uint32_t GPU_WAVERAM_WORDS_MASK = 0x3fffff;

// the frame buffer's words wrap at 1M words, as the software clears do
constexpr uint32_t GPU_FRAME_WORDS_MASK = 0xfffff;

// the chip's own depth range for a frame buffer word
constexpr float GPU_DEPTH_SCALE = 16777216.0f;

// consecutive commands above this count are sent without a read-back, so the queue stays short
constexpr size_t GPU_FLUSH_COMMANDS = 32768;

// the blend of a draw that combines the pixel the target holds with the shader's output. The alpha channel keeps the fragment's
// alpha, so the target's alpha does not change, except for the alpha texture, whose fragment alpha is its blend factor.
osd::gpu_blend_state gpu_blend(osd::gpu_blend_factor src, osd::gpu_blend_factor dst)
{
	osd::gpu_blend_state blend;
	blend.enable = true;
	blend.src = src;
	blend.dst = dst;
	blend.separate_alpha = true;
	blend.src_alpha = osd::gpu_blend_factor::ONE;
	blend.dst_alpha = osd::gpu_blend_factor::ZERO;
	return blend;
}

bool gpu_same_blend(const osd::gpu_blend_state &a, const osd::gpu_blend_state &b)
{
	return a.enable == b.enable
		&& a.src == b.src
		&& a.dst == b.dst
		&& a.constant_alpha == b.constant_alpha
		&& a.separate_alpha == b.separate_alpha
		&& a.src_alpha == b.src_alpha
		&& a.dst_alpha == b.dst_alpha;
}

} // anonymous namespace

bool zeus2_renderer::gpu_active()
{
	if (!m_gpu_checked)
		gpu_init();
	if (!m_gpu_ready)
		return false;

	// the second line mode changes the frame buffer's shape, which the target does not follow
	if (m_state->m_yScale != 0)
	{
		gpu_latch_off("the frame buffer changed shape (second line mode)");
		return false;
	}

	// the debug switch MAME_ZEUS2_FORCE_SOFTWARE_FRAMES=<frame> latches the host GPU path off at that frame
	static const char *const force_frame = osd_getenv("MAME_ZEUS2_FORCE_SOFTWARE_FRAMES");
	if (force_frame && m_state->screen().frame_number() == uint32_t(strtoul(force_frame, nullptr, 10)))
	{
		gpu_latch_off("MAME_ZEUS2_FORCE_SOFTWARE_FRAMES");
		return false;
	}
	return true;
}

bool zeus2_renderer::gpu_init()
{
	m_gpu_checked = true;

	// the other two Zeus 2 games keep the software renderer
	if (m_state->m_system != zeus2_device::CRUSNEXO)
		return false;

	m_gpu_engine = m_state->machine().osd().get_gpu_engine("zeus2");
	if (!m_gpu_engine)
		return false;
	if (m_gpu_engine->caps().max_texture_size < GPU_TEXTURE_WIDTH || m_gpu_engine->caps().max_texture_size < GPU_TEXTURE_ROWS)
	{
		osd_printf_verbose("zeus2: host GPU path unavailable, the largest texture is %d texels and %d are needed\n", m_gpu_engine->caps().max_texture_size, GPU_TEXTURE_WIDTH);
		m_gpu_engine = nullptr;
		return false;
	}

	// the options are read when the machine starts; the target is reduced to the largest scale whose rows the engine accepts
	m_gpu_msaa = m_state->machine().osd().msaa();
	m_gpu_scale = std::clamp(m_state->machine().osd().supersample(), 1, 3);
	while (m_gpu_scale > 1 && GPU_TARGET_ROWS * m_gpu_scale > m_gpu_engine->caps().max_texture_size)
		m_gpu_scale--;

	// a sample is taken inside the quad for a pixel the quad only partly covers, so the texture coordinates are never extrapolated
	osd::gpu_shader_module module;
	module.name = "zeus2";
	module.fragment_source = ZEUS2_FRAGMENT_SOURCE;
	module.centroid_mask = (m_gpu_msaa > 0) ? ((1U << ZEUS2_SHADER_VARYINGS) - 1) : 0;
	module.varying_count = ZEUS2_SHADER_VARYINGS;
	for (int i = 0; i < ZEUS2_SHADER_VARYINGS; i++)
		module.varying_mode[i] = osd::gpu_varying_mode::NOPERSPECTIVE;
	module.state_stride = ZEUS2_SHADER_STATE_WORDS;
	module.writes_depth = true;
	module.binding_count = 2;
	module.binding_name[0] = "tex_ram";
	module.binding_format[0] = osd::gpu_texture_format::R32UI;
	module.binding_name[1] = "tex_pal";
	module.binding_format[1] = osd::gpu_texture_format::R32UI;
	m_gpu_module = m_gpu_engine->create_shader_module(module);

	osd::gpu_texture_desc ram_desc;
	ram_desc.format = osd::gpu_texture_format::R32UI;
	ram_desc.width = GPU_TEXTURE_WIDTH;
	ram_desc.height = GPU_TEXTURE_ROWS;
	m_gpu_tex_ram = m_gpu_engine->create_texture(ram_desc);

	osd::gpu_texture_desc pal_desc;
	pal_desc.format = osd::gpu_texture_format::R32UI;
	pal_desc.width = 256;
	pal_desc.height = 1;
	m_gpu_tex_pal = m_gpu_engine->create_texture(pal_desc);

	if (!m_gpu_module || !m_gpu_tex_ram || !m_gpu_tex_pal)
	{
		m_gpu_engine = nullptr;
		return false;
	}

	m_gpu_row_dirty.assign(GPU_TEXTURE_ROWS, 0);
	m_gpu_ready = true;
	gpu_reset();
	osd_printf_info("zeus2: host GPU path on, target %dx%d, %dx supersampling, %d MSAA samples\n",
			GPU_TARGET_WIDTH * m_gpu_scale, GPU_TARGET_ROWS * m_gpu_scale, m_gpu_scale, m_gpu_msaa);
	return true;
}

// Called when the picture the target holds is not valid: a reset or a state load. The target is cleared at the next submit, all of
// wave RAM and the palette are uploaded again, and the frame buffer is not restored: the game redraws it, since each frame starts
// with a fast clear.
void zeus2_renderer::gpu_reset()
{
	m_gpu_generation++;
	m_gpu_commands.clear();
	m_gpu_vertices.clear();
	m_gpu_state.clear();
	m_gpu_arena.clear();
	m_gpu_patches.clear();
	m_gpu_clip_valid = false;
	m_gpu_target_diverged = false;
	if (m_gpu_ready)
	{
		std::fill(m_gpu_row_dirty.begin(), m_gpu_row_dirty.end(), uint8_t(1));
		m_gpu_row_min = 0;
		m_gpu_row_max = GPU_TEXTURE_ROWS - 1;
		m_gpu_palette_dirty = true;
	}
}

// The path stays off for the rest of the session. The polygons queued for the frame being built are dropped.
void zeus2_renderer::gpu_latch_off(const char *reason)
{
	if (!m_gpu_ready)
		return;
	osd_printf_info("zeus2: host GPU path off for the rest of the session: %s\n", reason);
	m_gpu_ready = false;
	m_gpu_engine = nullptr;
	m_gpu_clip_valid = false;
	m_gpu_commands.clear();
	m_gpu_vertices.clear();
	m_gpu_state.clear();
	m_gpu_arena.clear();
	m_gpu_patches.clear();
}

// A CPU read of the frame buffer. The CPU's copy is exact for everything except polygons drawn on the target, since fills and
// frame writes update it as well. A read after a polygon cannot be answered, so the path stops there; a read before any polygon
// (the boot frame-buffer test) is answered from the copy.
void zeus2_renderer::gpu_frame_read_hook()
{
	if (m_gpu_ready && m_gpu_target_diverged)
		gpu_latch_off("CPU frame-buffer read after polygons");
}

// A CPU write to wave RAM 0 at word index word (count words): their texture rows are uploaded before the next polygon.
void zeus2_renderer::gpu_mark_wave_ram(uint32_t word, uint32_t count)
{
	if (!m_gpu_ready)
		return;
	for (uint32_t i = 0; i < count; i++)
	{
		const int row = int(((word + i) & GPU_WAVERAM_WORDS_MASK) / GPU_TEXTURE_WIDTH);
		m_gpu_row_dirty[row] = 1;
		m_gpu_row_min = std::min(m_gpu_row_min, row);
		m_gpu_row_max = std::max(m_gpu_row_max, row);
	}
}

// A palette table load: the table is uploaded before the next polygon.
void zeus2_renderer::gpu_palette_changed()
{
	if (m_gpu_ready)
		m_gpu_palette_dirty = true;
}

// Wave RAM rows changed since the last upload, copied into the command list at this point. Consecutive rows make one update.
void zeus2_renderer::gpu_flush_wave()
{
	if (m_gpu_row_min > m_gpu_row_max)
		return;

	const uint32_t *const ram = m_state->m_waveram.get();
	int row = m_gpu_row_min;
	while (row <= m_gpu_row_max)
	{
		if (!m_gpu_row_dirty[row])
		{
			row++;
			continue;
		}
		int end = row;
		while (end <= m_gpu_row_max && m_gpu_row_dirty[end])
			m_gpu_row_dirty[end++] = 0;

		const size_t offset = m_gpu_arena.size();
		m_gpu_arena.insert(m_gpu_arena.end(), ram + size_t(row) * GPU_TEXTURE_WIDTH, ram + size_t(end) * GPU_TEXTURE_WIDTH);

		osd::gpu_texture_update update;
		update.texture = m_gpu_tex_ram;
		update.x = 0;
		update.y = row;
		update.w = GPU_TEXTURE_WIDTH;
		update.h = end - row;
		m_gpu_patches.emplace_back(m_gpu_commands.size(), offset);
		m_gpu_commands.push_back(osd::gpu_command::make_update(update));
		row = end;
	}
	m_gpu_row_min = GPU_TEXTURE_ROWS;
	m_gpu_row_max = -1;
}

// The palette table, copied into the command list at this point when a load changed it.
void zeus2_renderer::gpu_flush_palette()
{
	if (!m_gpu_palette_dirty)
		return;
	m_gpu_palette_dirty = false;

	const size_t offset = m_gpu_arena.size();
	m_gpu_arena.insert(m_gpu_arena.end(), m_state->m_pal_table, m_state->m_pal_table + 256);

	osd::gpu_texture_update update;
	update.texture = m_gpu_tex_pal;
	update.x = 0;
	update.y = 0;
	update.w = 256;
	update.h = 1;
	m_gpu_patches.emplace_back(m_gpu_commands.size(), offset);
	m_gpu_commands.push_back(osd::gpu_command::make_update(update));
}

// Sets the scissor to a rectangle of the target, in target pixels (the scale is applied here).
void zeus2_renderer::gpu_set_clip(int x, int y, int w, int h)
{
	if (m_gpu_clip_valid && x == m_gpu_clip_x && y == m_gpu_clip_y && w == m_gpu_clip_w && h == m_gpu_clip_h)
		return;
	m_gpu_clip_valid = true;
	m_gpu_clip_x = x;
	m_gpu_clip_y = y;
	m_gpu_clip_w = w;
	m_gpu_clip_h = h;
	m_gpu_commands.push_back(osd::gpu_command::make_clip(x * m_gpu_scale, y * m_gpu_scale, w * m_gpu_scale, h * m_gpu_scale));
}

// Adds a fill-kind quad (the solid colour, the raw depth word in v2) covering target pixels x0 to x1 - 1 of rows y0 to y1 - 1.
void zeus2_renderer::gpu_add_rect(int x0, int x1, int y0, int y1, uint32_t colour, int32_t depth, bool write_colour, bool write_depth)
{
	uint32_t words[SW_COUNT] = {};
	words[SW_KIND] = GPU_KIND_FILL;
	words[SW_COLOR] = colour & 0xffffff;
	const uint32_t prim = m_gpu_state.size() / SW_COUNT;
	m_gpu_state.insert(m_gpu_state.end(), words, words + SW_COUNT);

	const float scale = float(m_gpu_scale);
	auto corner = [prim, depth, scale](float x, float y)
	{
		gpu_vertex v = {};
		v.x = x * scale;
		v.y = y * scale;
		v.z = 0.0f;
		v.w = 1.0f;
		v.prim = prim;
		v.v[2] = float(depth);
		v.v[3] = 1.0f;
		return v;
	};
	const gpu_vertex a = corner(float(x0), float(y0));
	const gpu_vertex b = corner(float(x1), float(y0));
	const gpu_vertex c = corner(float(x1), float(y1));
	const gpu_vertex d = corner(float(x0), float(y1));
	const uint32_t first = m_gpu_vertices.size();
	m_gpu_vertices.insert(m_gpu_vertices.end(), { a, b, c, a, c, d });

	osd::gpu_command draw = osd::gpu_command::make_draw(m_gpu_module, first, 6);
	draw.bindings[0] = m_gpu_tex_ram;
	draw.bindings[1] = m_gpu_tex_pal;
	draw.depth_func = osd::gpu_depth_func::ALWAYS;
	draw.depth_write = write_depth;
	draw.color_write = write_colour;
	m_gpu_commands.push_back(draw);
}

// A fill of the frame buffer words [first, last) of the flat array, as the fast clear writes them. The words are split into a head
// row part, whole rows and a tail row part; words past the target's rows are not kept.
void zeus2_renderer::gpu_fill_words(uint32_t first, uint32_t last, uint32_t colour, int32_t depth)
{
	last = std::min(last, GPU_TARGET_WORDS);
	if (first >= last)
		return;
	gpu_set_clip(0, 0, GPU_TARGET_WIDTH, GPU_TARGET_ROWS);
	uint32_t pos = first;
	while (pos < last)
	{
		const int row = int(pos / GPU_TARGET_WIDTH);
		const int col = int(pos % GPU_TARGET_WIDTH);
		if (col == 0 && last - pos >= GPU_TARGET_WIDTH)
		{
			const int rows = int((last - pos) / GPU_TARGET_WIDTH);
			gpu_add_rect(0, GPU_TARGET_WIDTH, row, row + rows, colour, depth, true, true);
			pos += rows * GPU_TARGET_WIDTH;
		}
		else
		{
			const int x1 = int(std::min<uint32_t>(GPU_TARGET_WIDTH, col + (last - pos)));
			gpu_add_rect(col, x1, row, row + 1, colour, depth, true, true);
			pos += x1 - col;
		}
	}
	m_gpu_clip_valid = false;
}

// The fast clear of code 8: the words from the address, the count of them, wrapping at 1M words.
void zeus2_renderer::gpu_fill(uint32_t addr, uint32_t count, uint32_t colour, int32_t depth)
{
	if (!gpu_active())
		return;
	gpu_flush_wave();
	gpu_flush_palette();
	const uint32_t start = addr & GPU_FRAME_WORDS_MASK;
	const uint32_t end = start + count;
	if (end <= GPU_FRAME_WORDS_MASK + 1)
	{
		gpu_fill_words(start, end, colour, depth);
	}
	else
	{
		gpu_fill_words(start, GPU_FRAME_WORDS_MASK + 1, colour, depth);
		gpu_fill_words(0, end - (GPU_FRAME_WORDS_MASK + 1), colour, depth);
	}
}

// A CPU write to the frame buffer that the FIFO or code 9 makes with frame_write(): the colour of the word at addr (if write_colour0),
// the colour of the word after it (write_colour1) and the depth word at addr (write_depth). Each pixel becomes a one-pixel quad.
void zeus2_renderer::gpu_frame_write(uint32_t addr, bool write_colour0, uint32_t colour0, bool write_colour1, uint32_t colour1, bool write_depth, int32_t depth)
{
	if (!gpu_active())
		return;
	gpu_flush_wave();
	gpu_flush_palette();
	const uint32_t addr1 = (addr + 1) & GPU_FRAME_WORDS_MASK;
	if (write_colour0 || write_depth)
		gpu_write_pixel(addr & GPU_FRAME_WORDS_MASK, write_colour0, colour0, write_depth, depth);
	if (write_colour1)
		gpu_write_pixel(addr1, true, colour1, false, 0);
}

void zeus2_renderer::gpu_write_pixel(uint32_t word, bool write_colour, uint32_t colour, bool write_depth, int32_t depth)
{
	if (word >= GPU_TARGET_WORDS)
		return;
	gpu_set_clip(0, 0, GPU_TARGET_WIDTH, GPU_TARGET_ROWS);
	const int row = int(word / GPU_TARGET_WIDTH);
	const int col = int(word % GPU_TARGET_WIDTH);
	gpu_add_rect(col, col + 1, row, row + 1, colour, depth, write_colour, write_depth);
	m_gpu_clip_valid = false;
}

// Adds one polygon from zeus2_render_poly(): the clipped screen-space vertices as a fan of triangles, and the state record of the quad.
// Returns false when the polygon cannot go to the target (the path is then off).
bool zeus2_renderer::gpu_add_polygon(const zeus2_poly_extra_data &state, const vertex_t *vert, int numverts)
{
	// zclip_if_less() returns at most 8 vertices and zeus2_render_poly() drops anything under 3, so this cannot be hit
	if (numverts < 3 || numverts > 8)
		return true;

	// the software frame buffer would hold this polygon's pixels, the CPU copy does not
	m_gpu_target_diverged = true;

	// the window the polygon is drawn in: the frame base is the flat word of its origin (the row shift is 9 with a single line mode)
	const int row = int(state.frame_base >> 9);
	const int col = int(state.frame_base & 511);
	const rectangle &clip = m_state->zeus_cliprect;
	if (row + clip.max_y >= GPU_TARGET_ROWS)
	{
		gpu_latch_off("a polygon window is past the host target");
		return false;
	}

	gpu_flush_wave();
	gpu_flush_palette();

	// the solid colour is the RGB555 colour expanded, as the chip draws it
	const uint32_t solid = ((state.solidcolor & 0x7c00) << 9) | ((state.solidcolor & 0x3e0) << 6) | ((state.solidcolor & 0x1f) << 3);
	uint32_t words[SW_COUNT] = {};
	uint32_t mode = GPU_MODE_8BIT_2X2;
	if (state.solid_enable)
	{
		words[SW_KIND] = GPU_KIND_SOLID;
		words[SW_COLOR] = solid;
	}
	else
	{
		words[SW_KIND] = GPU_KIND_TEX;
		if (state.texture_rgb555)
			mode = GPU_MODE_RGB555;
		else if (state.texture_alpha)
			mode = GPU_MODE_ALPHA;
		else if (state.get_texel == &zeus2_device::get_texel_4bit_2x2)
			mode = GPU_MODE_4BIT;
		else if (state.get_texel == &zeus2_device::get_texel_8bit_4x2)
			mode = GPU_MODE_8BIT_4X2;
		words[SW_MODE] = mode;
		words[SW_TEXBASE] = uint32_t((state.texbase - m_state->m_waveram.get()) * 4);
		words[SW_TEXWIDTH] = state.texwidth;
		// the colour key of the chip's pixel path: the alpha texture has none
		words[SW_TRANSKEY] = (state.transcolor == 0 && mode != GPU_MODE_ALPHA) ? 1 : 0;
	}
	words[SW_DEPTH_CLEAR] = state.depth_clear_enable ? 1 : 0;
	words[SW_ZMIN] = uint32_t(state.zbuf_min);

	// the blend of the chip's pixel path: a solid or opaque textured pixel is scaled by its source alpha and added to the destination
	// scaled by the destination alpha (the draw's constant); an alpha texture blends by its own alpha; the RGB555 path does not blend
	osd::gpu_blend_state blend_state;
	if (mode == GPU_MODE_ALPHA)
	{
		blend_state = gpu_blend(osd::gpu_blend_factor::ONE, osd::gpu_blend_factor::ONE_MINUS_SRC_ALPHA);
	}
	else if (mode != GPU_MODE_RGB555 && state.blend_enable)
	{
		words[SW_BLEND] = 1;
		words[SW_SRCA] = state.srcAlpha;
		blend_state = gpu_blend(osd::gpu_blend_factor::ONE, osd::gpu_blend_factor::CONSTANT_ALPHA);
		blend_state.constant_alpha = float(std::min<uint32_t>(state.dstAlpha, 0x100)) / 256.0f;
	}

	// the RGB555 path writes no depth; the depth test still runs
	const bool depth_test = state.depth_test_enable;
	const bool depth_write = state.depth_write_enable && mode != GPU_MODE_RGB555;
	const osd::gpu_depth_func func = depth_test ? osd::gpu_depth_func::LEQUAL : osd::gpu_depth_func::ALWAYS;

	// texture coordinates in the chip's units: u / z and v / z, the depth in 12.12 and 1 / z (zeus2_shader.h)
	const float scale = float(m_gpu_scale);
	const uint32_t prim = m_gpu_state.size() / SW_COUNT;
	gpu_vertex vv[8];
	for (int i = 0; i < numverts; i++)
	{
		vv[i].x = (vert[i].x + col) * scale;
		vv[i].y = (vert[i].y + row) * scale;
		vv[i].z = 0.0f;
		vv[i].w = 1.0f;
		vv[i].prim = prim;
		vv[i].v[0] = vert[i].p[1];
		vv[i].v[1] = vert[i].p[2];
		vv[i].v[2] = vert[i].p[0];
		vv[i].v[3] = vert[i].p[3];
	}
	m_gpu_state.insert(m_gpu_state.end(), words, words + SW_COUNT);

	const uint32_t first = m_gpu_vertices.size();
	for (int i = 1; i + 1 < numverts; i++)
	{
		m_gpu_vertices.push_back(vv[0]);
		m_gpu_vertices.push_back(vv[i]);
		m_gpu_vertices.push_back(vv[i + 1]);
	}
	const uint32_t count = m_gpu_vertices.size() - first;

	// the clip is the chip's clip rectangle moved to the window
	gpu_set_clip(col + clip.min_x, row + clip.min_y, clip.width(), clip.height());

	// consecutive polygons with the same depth, write and blend state, and nothing between them, share one draw
	if (!m_gpu_commands.empty())
	{
		osd::gpu_command &last = m_gpu_commands.back();
		if (last.type == osd::gpu_command_type::DRAW && last.module == m_gpu_module && last.first_vertex + last.vertex_count == first && last.depth_func == func
				&& last.depth_write == depth_write && last.color_write && gpu_same_blend(last.blend, blend_state))
		{
			last.vertex_count += count;
			return true;
		}
	}
	osd::gpu_command draw = osd::gpu_command::make_draw(m_gpu_module, first, count);
	draw.bindings[0] = m_gpu_tex_ram;
	draw.bindings[1] = m_gpu_tex_pal;
	draw.depth_func = func;
	draw.depth_write = depth_write;
	draw.blend = blend_state;
	m_gpu_commands.push_back(draw);

	if (m_gpu_commands.size() > GPU_FLUSH_COMMANDS && !gpu_submit(nullptr, true))
		gpu_latch_off("the engine refused a frame");
	return true;
}

// Sends the queued frame. A read-back is written by this call; flush sends the commands without one, to keep the queue short.
bool zeus2_renderer::gpu_submit(const osd::gpu_readback_rect *readback, bool flush)
{
	gpu_flush_wave();
	gpu_flush_palette();

	// the update commands point into the arena, which is complete now
	for (const auto &patch : m_gpu_patches)
		m_gpu_commands[patch.first].update.data = m_gpu_arena.data() + patch.second;

	osd::gpu_frame frame;
	frame.target.width = GPU_TARGET_WIDTH * m_gpu_scale;
	frame.target.height = GPU_TARGET_ROWS * m_gpu_scale;
	frame.target.persistent = true;
	frame.target.depth = osd::gpu_depth_mode::REAL;
	frame.target.msaa_samples = m_gpu_msaa;
	frame.target.generation = m_gpu_generation;
	frame.vertices = m_gpu_vertices.data();
	frame.vertex_count = m_gpu_vertices.size();
	frame.varyings_per_vertex = ZEUS2_SHADER_VARYINGS;
	frame.prim_state = m_gpu_state.data();
	frame.prim_count = m_gpu_state.size() / SW_COUNT;
	frame.state_stride = ZEUS2_SHADER_STATE_WORDS;
	frame.commands = m_gpu_commands.data();
	frame.command_count = int(m_gpu_commands.size());
	frame.readbacks = readback;
	frame.readback_count = readback ? 1 : 0;

	const bool ok = flush ? m_gpu_engine->flush_frame(frame) : m_gpu_engine->submit_frame(frame);

	m_gpu_commands.clear();
	m_gpu_vertices.clear();
	m_gpu_state.clear();
	m_gpu_arena.clear();
	m_gpu_patches.clear();
	m_gpu_clip_valid = false;
	return ok;
}

// Draws the frame's polygons and reads back the rows of the cliprect from the display base (0x38), as the software copy does. The
// page x of a screen pixel is its x. Returns false when the path is off, or when the screen is not on the target, which latches the
// path off; the software copy then shows the frame buffer as it stands.
bool zeus2_renderer::gpu_screen_update(bitmap_rgb32 &bitmap, const rectangle &cliprect)
{
	if (!gpu_active())
		return false;

	const uint32_t display = m_state->frame_addr_from_xy(0, 0, false);
	if (display & (GPU_TARGET_WIDTH - 1))
	{
		gpu_latch_off("the display base is not on a row");
		return false;
	}
	const int row0 = int(display / GPU_TARGET_WIDTH);
	if (row0 + cliprect.max_y >= GPU_TARGET_ROWS)
	{
		gpu_latch_off("the display rows are past the host target");
		return false;
	}

	osd::gpu_readback_rect readback;
	readback.x = cliprect.min_x * m_gpu_scale;
	readback.y = (row0 + cliprect.min_y) * m_gpu_scale;
	readback.w = cliprect.width() * m_gpu_scale;
	readback.h = cliprect.height() * m_gpu_scale;
	readback.downsample = m_gpu_scale;
	readback.out = &bitmap.pix(cliprect.min_y, cliprect.min_x);
	readback.stride = bitmap.rowpixels();
	readback.opaque = true;

	if (!gpu_submit(&readback, false))
	{
		gpu_latch_off("the engine refused a frame");
		return false;
	}
	return true;
}
