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

	osd::gpu_shader_module module;
	module.name = "midvunit";
	module.fragment_source = MIDVUNIT_FRAGMENT_SOURCE;
	module.varying_count = MIDVUNIT_SHADER_VARYINGS;
	for (int i = 0; i < MIDVUNIT_SHADER_VARYINGS; i++)
		module.varying_mode[i] = osd::gpu_varying_mode::NOPERSPECTIVE;
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
	osd_printf_verbose("midvunit: host GPU path on, target %dx%d\n", GPU_PAGE_WIDTH, GPU_TARGET_HEIGHT);
	return true;
}

// Called when the frame buffer state the target holds can no longer be trusted: a state load, a reset, or the
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

// The target no longer matches the CPU's view of the frame buffer (the CPU read or wrote it, which the adapter does not
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

// Adds one quad the way the scanline renderer would draw it: the same vertices (already made inclusive), the same
// palette base and mode. The two triangles split the quad along the diagonal from vertex 0 to vertex 2.
void midvunit_renderer::gpu_add_quad(const vertex_t *vert, uint16_t pixdata, bool textured, uint16_t flags, uint16_t texpage)
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

	// the draw page is stacked below the display page's rows of the target
	const int page = (m_state.m_page_control & 4) ? GPU_PAGE_ROWS : 0;
	if (page != m_gpu_clip_page)
	{
		const rectangle &vis = m_state.m_screen->visible_area();
		m_gpu_commands.push_back(osd::gpu_command::make_clip(vis.min_x, page + vis.min_y, vis.width(), vis.height()));
		m_gpu_clip_page = page;
	}

	const uint32_t prim = m_gpu_state.size() / MIDVUNIT_SHADER_STATE_WORDS;
	m_gpu_state.push_back(mode);
	m_gpu_state.push_back(pixdata);
	m_gpu_state.push_back(textured ? uint32_t(texpage) * 256 : 0);
	m_gpu_state.push_back((flags & 0x2000) != 0);

	// the texture coordinates are 16.16 values with the half texel offset; the shader takes them in texel units
	gpu_vertex vv[4];
	for (int i = 0; i < 4; i++)
	{
		vv[i].x = vert[i].x;
		vv[i].y = vert[i].y + page;
		vv[i].z = 0.0f;
		vv[i].w = 1.0f;
		vv[i].prim = prim;
		vv[i].v[0] = textured ? vert[i].p[0] / 65536.0f : 0.0f;
		vv[i].v[1] = textured ? vert[i].p[1] / 65536.0f : 0.0f;
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

// Draws the frame's quads into the target and, when the display page changed, reads the page on screen back into the bitmap.
// Returns true when the bitmap holds a new picture. On a refusal by the engine the path is latched off and the picture
// of this frame is left as it was; the next frame is drawn in software.
bool midvunit_renderer::gpu_screen_update(bitmap_rgb32 &bitmap, bool changed)
{
	const rectangle &vis = m_state.m_screen->visible_area();
	if (!m_gpu_ready)
		return false;
	if (bitmap.width() <= vis.max_x || bitmap.height() <= vis.max_y)
		return false;

	// the display page starts at the second page of the target when the display bit is set
	const int display = (m_state.m_page_control & 1) ? GPU_PAGE_ROWS : 0;
	osd::gpu_readback_rect readback;
	readback.x = vis.min_x;
	readback.y = display + vis.min_y;
	readback.w = vis.width();
	readback.h = vis.height();
	readback.out = &bitmap.pix(vis.min_y, vis.min_x);
	readback.stride = bitmap.rowpixels();
	readback.opaque = true;

	osd::gpu_frame frame;
	frame.target.width = GPU_PAGE_WIDTH;
	frame.target.height = GPU_TARGET_HEIGHT;
	frame.target.persistent = true;
	frame.target.depth = osd::gpu_depth_mode::NONE;
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
