// license:BSD-3-Clause
// copyright-holders:TrickoUK
/***************************************************************************

    Midway Zeus 1 host-GPU path (included by midzeus_v.cpp)

    The polygons that render_polygon() would rasterise become one triangle
    list in the same order, with a state record per polygon for the colour
    and depth rules of the chip. The frame buffer is one persistent target,
    512 pixels wide and 1024 rows tall: a polygon is drawn at the row its
    render base names, and the display base picks the rows read back. Wave RAM 0 (textures and palettes) is
    mirrored as one byte texture, and every CPU write to it goes into the
    command list at its point in the frame, so a polygon samples the bytes
    as they stood when the chip drew it. The page on screen comes back as
    ordinary pixels when the screen is updated.

    The CPU writes to the frame buffer are not copied into the target, so the
    target keeps the pixels the chip drew. A CPU read of the frame buffer
    latches the path off for the rest of the session.

***************************************************************************/

namespace {

// state record words, as the shader reads them (midzeus_shader.h)
enum : uint32_t
{
	SW_KIND = 0,
	SW_COLOR,
	SW_TEXBASE,
	SW_TEXWIDTH,
	SW_LAYOUT,
	SW_VOFFSET,
	SW_PALBASE,
	SW_KEY,
	SW_BLEND,
	SW_SRCA,
	SW_ZOFFSET,
	SW_DEPTH_TEST,
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
	GPU_LAYOUT_4BIT = 0,
	GPU_LAYOUT_8BIT,
	GPU_LAYOUT_ALT_4BIT,
	GPU_LAYOUT_ALT_8BIT
};

enum : uint32_t
{
	GPU_BLEND_PLAIN = 0,
	GPU_BLEND_OPAQUE_FACTOR,
	GPU_BLEND_SRCA_FACTOR
};

static_assert(SW_COUNT == MIDZEUS_SHADER_STATE_WORDS, "state record size");

// the frame buffer is 2 MB: 512 pixels per row, each with 16 bits of colour and 16 bits of depth, so a row is 2048 bytes and
// there are 1024 rows
constexpr int GPU_TARGET_WIDTH = 512;
constexpr int GPU_TARGET_ROWS = 1024;
constexpr int GPU_ROW_BYTES = 2048;

// wave RAM 0 as bytes: 512 blocks of 8 bytes per row, 2048 rows
constexpr int GPU_TEXTURE_WIDTH = 4096;
constexpr int GPU_TEXTURE_HEIGHT = 2048;
constexpr int GPU_BLOCK_BYTES = 8;

} // anonymous namespace

bool midzeus_renderer::gpu_active()
{
	if (!m_gpu_checked)
		gpu_init();
	return m_gpu_ready;
}

bool midzeus_renderer::gpu_init()
{
	m_gpu_checked = true;
	m_gpu_engine = m_state.machine().osd().get_gpu_engine("midzeus");
	if (!m_gpu_engine)
		return false;
	if (m_gpu_engine->caps().max_texture_size < GPU_TEXTURE_WIDTH || m_gpu_engine->caps().max_texture_size < GPU_TEXTURE_HEIGHT)
	{
		osd_printf_verbose("midzeus: host GPU path unavailable, the largest texture is %d texels and %d are needed\n", m_gpu_engine->caps().max_texture_size, GPU_TEXTURE_WIDTH);
		m_gpu_engine = nullptr;
		return false;
	}

	osd::gpu_shader_module module;
	module.name = "midzeus";
	module.fragment_source = MIDZEUS_FRAGMENT_SOURCE;
	module.varying_count = MIDZEUS_SHADER_VARYINGS;
	for (int i = 0; i < MIDZEUS_SHADER_VARYINGS; i++)
		module.varying_mode[i] = osd::gpu_varying_mode::NOPERSPECTIVE;
	module.state_stride = MIDZEUS_SHADER_STATE_WORDS;
	module.writes_depth = true;
	module.binding_count = 1;
	module.binding_name[0] = "tex_ram";
	module.binding_format[0] = osd::gpu_texture_format::R8UI;
	m_gpu_module = m_gpu_engine->create_shader_module(module);

	osd::gpu_texture_desc desc;
	desc.format = osd::gpu_texture_format::R8UI;
	desc.width = GPU_TEXTURE_WIDTH;
	desc.height = GPU_TEXTURE_HEIGHT;
	m_gpu_texture = m_gpu_engine->create_texture(desc);

	if (!m_gpu_module || !m_gpu_texture)
	{
		m_gpu_engine = nullptr;
		return false;
	}

	m_gpu_texture_dirty = std::make_unique<osd::gpu_dirty_region>(GPU_TEXTURE_WIDTH, GPU_TEXTURE_HEIGHT);
	m_gpu_ready = true;
	gpu_reset();
	osd_printf_info("midzeus: host GPU path on, target %dx%d\n", GPU_TARGET_WIDTH, GPU_TARGET_ROWS);
	return true;
}

// Called when the frame buffer the target holds can no longer be trusted: a state load or a reset. The target is cleared at the next
// frame, and wave RAM 0 is uploaded again in full.
void midzeus_renderer::gpu_reset()
{
	m_gpu_generation++;
	if (m_gpu_texture_dirty)
		m_gpu_texture_dirty->mark_all();
	m_gpu_clip_row = -1;
	m_gpu_commands.clear();
	m_gpu_vertices.clear();
	m_gpu_state.clear();
	m_gpu_blobs.clear();
}

// The path stays off for the rest of the session. The polygons queued for the frame being built are dropped.
void midzeus_renderer::gpu_latch_off(const char *reason)
{
	if (!m_gpu_ready)
		return;
	osd_printf_verbose("midzeus: host GPU path off for the rest of the session: %s\n", reason);
	m_gpu_ready = false;
	m_gpu_engine = nullptr;
	m_gpu_clip_row = -1;
	m_gpu_commands.clear();
	m_gpu_vertices.clear();
	m_gpu_state.clear();
	m_gpu_blobs.clear();
}

// A CPU write to the 8-byte block of wave RAM 0 at block. The bytes are uploaded before the next polygon.
void midzeus_renderer::gpu_mark_texture(const void *block)
{
	if (!m_gpu_texture_dirty)
		return;
	const uint32_t offset = uint32_t(uintptr_t(block) - uintptr_t(m_state.m_waveram[0].get()));
	m_gpu_texture_dirty->mark(offset % GPU_TEXTURE_WIDTH, offset / GPU_TEXTURE_WIDTH, GPU_BLOCK_BYTES, 1);
}

// The game flips the render base (0x84) or the display base (0xcc). The queued polygons are sent now, so the queue stays
// short on frames that are not shown; the rest of the frame goes out with screen_update().
void midzeus_renderer::gpu_flip()
{
	if (!m_gpu_ready || m_gpu_commands.empty())
		return;
	if (!gpu_submit(nullptr, true))
		gpu_latch_off("the engine refused a frame");
}

// The first row of the frame buffer a block pointer names.
int midzeus_renderer::gpu_page_row(const void *block) const
{
	return int((uintptr_t(block) - uintptr_t(m_state.m_waveram[1].get())) / GPU_ROW_BYTES);
}

// The texture bytes that changed since the last upload, copied into the command list at this point.
void midzeus_renderer::gpu_flush_textures()
{
	int x, y, w, h;
	if (!m_gpu_texture_dirty->take(x, y, w, h))
		return;

	auto &blob = m_gpu_blobs.emplace_back(size_t(w) * h);
	const uint8_t *const ram = (const uint8_t *)m_state.m_waveram[0].get();
	for (int row = 0; row < h; row++)
		memcpy(&blob[size_t(row) * w], ram + size_t(y + row) * GPU_TEXTURE_WIDTH + x, w);

	osd::gpu_texture_update update;
	update.texture = m_gpu_texture;
	update.x = x;
	update.y = y;
	update.w = w;
	update.h = h;
	update.data = blob.data();
	m_gpu_commands.push_back(osd::gpu_command::make_update(update));
}

// Sets the scissor to the clip rectangle of a page. The clip is in page coordinates, so the page's first row is added.
void midzeus_renderer::gpu_set_clip(int row, const rectangle &clip)
{
	if (row == m_gpu_clip_row && clip.min_x == m_gpu_clip.min_x && clip.max_x == m_gpu_clip.max_x && clip.min_y == m_gpu_clip.min_y && clip.max_y == m_gpu_clip.max_y)
		return;
	m_gpu_clip_row = row;
	m_gpu_clip = clip;
	m_gpu_commands.push_back(osd::gpu_command::make_clip(clip.min_x, row + clip.min_y, clip.width(), clip.height()));
}

// Adds one polygon from capture_polygon(): the clipped screen-space vertices as a fan of triangles, and the state record of the
// quad (or of the solid fill, which takes the colour and the depth from the same record and no texture).
void midzeus_renderer::gpu_add_polygon(const rectangle &clip, const vertex_t *vert, uint32_t numverts, const mz_poly_extra_data &state, bool fill)
{
	if (numverts < 3 || numverts > 6)
		return;

	uint32_t words[SW_COUNT] = {};
	const uint32_t ctrl = state.ctrl_word;
	if (fill)
	{
		words[SW_KIND] = GPU_KIND_FILL;
	}
	else if ((ctrl & 0x000c0000) == 0x000c0000)
	{
		words[SW_KIND] = GPU_KIND_SOLID;
	}
	else if (ctrl & 0x01000000)
	{
		words[SW_KIND] = GPU_KIND_TEX;
	}
	else
	{
		// a quad with neither a texture nor a solid colour has no source for its pixels
		return;
	}

	// the blend modes of render_poly(): the OPAQUE modes that keep the source colour, the two that scale it, and the modes that
	// read the destination pixel, which are not drawn
	uint32_t blend = GPU_BLEND_PLAIN;
	if (!fill)
	{
		switch (state.blend)
		{
			case BLEND_OPAQUE2:
			case BLEND_OPAQUE3:
				blend = GPU_BLEND_OPAQUE_FACTOR;
				break;

			case BLEND_OPAQUE5:
				blend = GPU_BLEND_SRCA_FACTOR;
				break;

			case BLEND_ADD1:
			case BLEND_ADD2:
			case BLEND_MUL1:
				return;

			default:
				break;
		}
	}

	words[SW_COLOR] = state.solidcolor & 0x7fff;
	if (words[SW_KIND] == GPU_KIND_TEX)
	{
		const uint8_t *const waveram0 = (const uint8_t *)m_state.m_waveram[0].get();
		words[SW_TEXBASE] = uint32_t((const uint8_t *)state.texbase - waveram0);
		words[SW_TEXWIDTH] = state.texwidth;
		words[SW_VOFFSET] = state.voffset;
		words[SW_PALBASE] = uint32_t((const uint8_t *)state.palbase - waveram0);
		if (state.get_texel == get_texel_4bit)
			words[SW_LAYOUT] = GPU_LAYOUT_4BIT;
		else if (state.get_texel == get_texel_8bit)
			words[SW_LAYOUT] = GPU_LAYOUT_8BIT;
		else if (state.get_texel == get_texel_alt_4bit)
			words[SW_LAYOUT] = GPU_LAYOUT_ALT_4BIT;
		else
			words[SW_LAYOUT] = GPU_LAYOUT_ALT_8BIT;
	}
	words[SW_KEY] = (!fill && state.transcolor == 0) ? 1 : 0;
	words[SW_BLEND] = blend;
	words[SW_SRCA] = state.alpha & 0xff;
	words[SW_ZOFFSET] = uint32_t(int32_t(state.zoffset));

	// the fill is an unconditional write of colour and depth, as render_poly_solid_fixedz() does it
	const bool depth_test = !fill && state.depth_test_enable;
	const bool depth_write = fill || state.depth_write_enable;
	words[SW_DEPTH_TEST] = depth_test ? 1 : 0;

	const int row = gpu_page_row(state.renderbase);
	gpu_flush_textures();
	gpu_set_clip(row, clip);

	// texture coordinates in texel units; the depth is the chip's 16.16 z, which the shader reduces to the integer depth
	const uint32_t prim = m_gpu_state.size() / SW_COUNT;
	gpu_vertex vv[6];
	for (uint32_t i = 0; i < numverts; i++)
	{
		vv[i].x = vert[i].x;
		vv[i].y = vert[i].y + row;
		vv[i].z = 0.0f;
		vv[i].w = 1.0f;
		vv[i].prim = prim;
		vv[i].v[0] = fill ? 0.0f : vert[i].p[1] / 256.0f;
		vv[i].v[1] = fill ? 0.0f : vert[i].p[2] / 256.0f;
		vv[i].v[2] = fill ? 0.0f : vert[i].p[0];
	}

	m_gpu_state.insert(m_gpu_state.end(), words, words + SW_COUNT);

	const uint32_t first = m_gpu_vertices.size();
	for (uint32_t i = 1; i + 1 < numverts; i++)
	{
		m_gpu_vertices.push_back(vv[0]);
		m_gpu_vertices.push_back(vv[i]);
		m_gpu_vertices.push_back(vv[i + 1]);
	}
	const uint32_t count = m_gpu_vertices.size() - first;

	const osd::gpu_depth_func func = depth_test ? osd::gpu_depth_func::LEQUAL : osd::gpu_depth_func::ALWAYS;

	// consecutive polygons with the same depth state, and nothing between them, share one draw
	if (!m_gpu_commands.empty())
	{
		osd::gpu_command &last = m_gpu_commands.back();
		if (last.type == osd::gpu_command_type::DRAW && last.first_vertex + last.vertex_count == first && last.depth_func == func && last.depth_write == depth_write)
		{
			last.vertex_count += count;
			return;
		}
	}
	osd::gpu_command draw = osd::gpu_command::make_draw(m_gpu_module, first, count);
	draw.bindings[0] = m_gpu_texture;
	draw.depth_func = func;
	draw.depth_write = depth_write;
	m_gpu_commands.push_back(draw);
}

// Sends the queued frame. A readback is written by this call; flush sends the commands without one, to keep the queue short.
bool midzeus_renderer::gpu_submit(const osd::gpu_readback_rect *readback, bool flush)
{
	osd::gpu_frame frame;
	frame.target.width = GPU_TARGET_WIDTH;
	frame.target.height = GPU_TARGET_ROWS;
	frame.target.persistent = true;
	frame.target.depth = osd::gpu_depth_mode::REAL;
	frame.target.generation = m_gpu_generation;
	frame.vertices = m_gpu_vertices.data();
	frame.vertex_count = m_gpu_vertices.size();
	frame.varyings_per_vertex = MIDZEUS_SHADER_VARYINGS;
	frame.prim_state = m_gpu_state.data();
	frame.prim_count = m_gpu_state.size() / SW_COUNT;
	frame.state_stride = MIDZEUS_SHADER_STATE_WORDS;
	frame.commands = m_gpu_commands.data();
	frame.command_count = int(m_gpu_commands.size());
	frame.readbacks = readback;
	frame.readback_count = readback ? 1 : 0;

	const bool ok = flush ? m_gpu_engine->flush_frame(frame) : m_gpu_engine->submit_frame(frame);

	m_gpu_commands.clear();
	m_gpu_vertices.clear();
	m_gpu_state.clear();
	m_gpu_blobs.clear();
	m_gpu_clip_row = -1;
	return ok;
}

// Draws the frame's polygons and reads back the rows of the cliprect that the screen shows, from the display base (0xcc). The
// page x of a screen pixel is its x less the visible area's left edge, as in the software copy. Returns false when the path is off,
// or when the engine refused the frame, which latches the path off; the software copy then shows the frame buffer as it stands.
bool midzeus_renderer::gpu_screen_update(bitmap_rgb32 &bitmap, const rectangle &cliprect)
{
	if (!gpu_active())
		return false;

	const rectangle &vis = m_state.m_screen->visible_area();
	const int32_t x0 = std::max(cliprect.min_x, vis.min_x);
	const int32_t x1 = std::min(cliprect.max_x, vis.max_x);
	const int32_t y0 = std::max(cliprect.min_y, vis.min_y);
	const int32_t y1 = std::min(cliprect.max_y, vis.max_y);
	const bool any = (x0 <= x1) && (y0 <= y1);

	const int display = gpu_page_row(m_state.waveram1_ptr_from_expanded_addr(m_state.m_zeusbase[0xcc]));
	osd::gpu_readback_rect readback;
	if (any)
	{
		readback.x = x0 - vis.min_x;
		readback.y = display + y0;
		readback.w = x1 - x0 + 1;
		readback.h = y1 - y0 + 1;
		readback.out = &bitmap.pix(y0, x0);
		readback.stride = bitmap.rowpixels();
		readback.opaque = true;
	}

	if (!gpu_submit(any ? &readback : nullptr, false))
	{
		gpu_latch_off("the engine refused a frame");
		return false;
	}
	return true;
}
