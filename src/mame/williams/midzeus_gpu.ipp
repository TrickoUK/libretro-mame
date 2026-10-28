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

    With mame_3d_supersample the target holds a block of samples for each
    native pixel, and the read-back averages each block back to one pixel.
    Every coordinate the target is addressed by is multiplied by the scale
    (the vertices, the clip rectangle and the stamps), while the frame
    buffer's words stay at native size. With mame_3d_msaa the target keeps
    coverage samples per pixel, resolved by the same read-back.

    The CPU writes to the frame buffer (0xb0/0xb2 with bit 31 of 0xb6 set)
    reach the target as write stamps. Each written word is flagged per pixel
    until the next draw or submit. Then the frame buffer's words are uploaded
    for the flagged blocks, and one stamp draw per plane and rectangle copies
    the flagged colour or depth words into the target, so the write lands at
    its point in the command list. Pixels the chip drew and the CPU did not
    write keep their drawn values. A CPU read of the frame buffer latches the
    path off for the rest of the session.

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
	SW_SCALE,
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
	GPU_BLEND_SRCA_FACTOR,
	GPU_BLEND_MUL_FACTOR
};

static_assert(SW_COUNT == MIDZEUS_SHADER_STATE_WORDS, "state record size");

// the mode word bit that enables the destination read of ADD1, ADD2 and MUL1 (render_poly())
constexpr uint32_t GPU_BLEND_DST_BIT = 0x00800000;

// the blend of a draw that combines the pixel the target holds with the shader's output. The alpha channel keeps the fragment's
// alpha of 1, so the target's alpha does not change.
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

// the frame buffer is 2 MB: 512 pixels per row, each with 16 bits of colour and 16 bits of depth, so a row is 2048 bytes and
// there are 1024 rows
constexpr int GPU_TARGET_WIDTH = 512;
constexpr int GPU_TARGET_ROWS = 1024;
constexpr int GPU_ROW_BYTES = 2048;

// wave RAM 0 as bytes: 512 blocks of 8 bytes per row, 2048 rows
constexpr int GPU_TEXTURE_WIDTH = 4096;
constexpr int GPU_TEXTURE_HEIGHT = 2048;
constexpr int GPU_BLOCK_BYTES = 8;

// the frame buffer as 16-bit words: a row is 1024 words, the mirror texture is the frame buffer as the chip holds it
constexpr int GPU_ROW_WORDS = GPU_ROW_BYTES / 2;

// per-pixel flags of the CPU writes not yet in the target: the colour word and the depth word of the pixel
enum : uint8_t
{
	GPU_FB_COLOUR = 1,
	GPU_FB_DEPTH = 2
};

// the variant of the stamp module: the colour plane or the depth plane of the mirror (midzeus_stamp.glsl)
enum : uint32_t
{
	GPU_STAMP_COLOUR = 0,
	GPU_STAMP_DEPTH = 1
};

// Calls emit(x0, x1, y0, y1) for rectangles of target pixels whose flags include mask: pixels x0 to x1 - 1 of rows y0 to y1 - 1.
// A run that matches the run on the row above continues that rectangle, so a region written in whole rows is one rectangle.
template <typename T>
void gpu_flag_rects(const std::vector<uint8_t> &flags, int row_min, int row_max, uint8_t mask, T &&emit)
{
	struct open_rect
	{
		int x0, x1, y0;
	};
	std::vector<open_rect> open;
	std::vector<std::pair<int, int> > runs;
	for (int y = row_min; y <= row_max + 1; y++)
	{
		// row_max + 1 holds no runs, which closes the rectangles still open
		runs.clear();
		if (y <= row_max)
		{
			const uint8_t *const row = &flags[size_t(y) * GPU_TARGET_WIDTH];
			for (int x = 0; x < GPU_TARGET_WIDTH; )
			{
				if (!(row[x] & mask))
				{
					x++;
					continue;
				}
				int x1 = x + 1;
				while (x1 < GPU_TARGET_WIDTH && (row[x1] & mask))
					x1++;
				runs.emplace_back(x, x1);
				x = x1;
			}
		}

		std::vector<open_rect> next;
		for (const open_rect &r : open)
		{
			const auto run = std::find(runs.begin(), runs.end(), std::make_pair(r.x0, r.x1));
			if (run == runs.end())
			{
				emit(r.x0, r.x1, r.y0, y);
			}
			else
			{
				runs.erase(run);
				next.push_back(r);
			}
		}
		for (const auto &run : runs)
			next.push_back({ run.first, run.second, y });
		open.swap(next);
	}
}

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

	// the options are read when the machine starts, as the core option text says they take effect on a restart; the target is
	// reduced to the largest scale whose rows the texture size allows
	m_gpu_msaa = m_state.machine().osd().msaa();
	m_gpu_scale = std::clamp(m_state.machine().osd().supersample(), 1, 3);
	while (m_gpu_scale > 1 && GPU_TARGET_ROWS * m_gpu_scale > int(m_gpu_engine->caps().max_texture_size))
		m_gpu_scale--;

	// a sample is taken inside the quad for a pixel the quad only partly covers, so u, v and z are never extrapolated past its
	// edges (z is the depth the shader writes for every covered sample of the pixel)
	osd::gpu_shader_module module;
	module.name = "midzeus";
	module.fragment_source = MIDZEUS_FRAGMENT_SOURCE;
	module.centroid_mask = (m_gpu_msaa > 0) ? 0x7 : 0;
	module.varying_count = MIDZEUS_SHADER_VARYINGS;
	for (int i = 0; i < MIDZEUS_SHADER_VARYINGS; i++)
		module.varying_mode[i] = osd::gpu_varying_mode::NOPERSPECTIVE;
	module.state_stride = MIDZEUS_SHADER_STATE_WORDS;
	module.writes_depth = true;
	module.binding_count = 1;
	module.binding_name[0] = "tex_ram";
	module.binding_format[0] = osd::gpu_texture_format::R8UI;
	m_gpu_module = m_gpu_engine->create_shader_module(module);

	osd::gpu_shader_module stamp;
	stamp.name = "midzeus_stamp";
	stamp.fragment_source = MIDZEUS_STAMP_SOURCE;
	stamp.varying_count = MIDZEUS_SHADER_VARYINGS;
	for (int i = 0; i < MIDZEUS_SHADER_VARYINGS; i++)
		stamp.varying_mode[i] = osd::gpu_varying_mode::NOPERSPECTIVE;
	stamp.state_stride = MIDZEUS_SHADER_STATE_WORDS;
	stamp.writes_depth = true;
	stamp.binding_count = 1;
	stamp.binding_name[0] = "tex_fb";
	stamp.binding_format[0] = osd::gpu_texture_format::R16UI;
	m_gpu_stamp_module = m_gpu_engine->create_shader_module(stamp);

	osd::gpu_texture_desc desc;
	desc.format = osd::gpu_texture_format::R8UI;
	desc.width = GPU_TEXTURE_WIDTH;
	desc.height = GPU_TEXTURE_HEIGHT;
	m_gpu_texture = m_gpu_engine->create_texture(desc);

	osd::gpu_texture_desc fb_desc;
	fb_desc.format = osd::gpu_texture_format::R16UI;
	fb_desc.width = GPU_ROW_WORDS;
	fb_desc.height = GPU_TARGET_ROWS;
	m_gpu_fb_texture = m_gpu_engine->create_texture(fb_desc);

	if (!m_gpu_module || !m_gpu_texture || !m_gpu_stamp_module || !m_gpu_fb_texture)
	{
		m_gpu_engine = nullptr;
		return false;
	}

	m_gpu_texture_dirty = std::make_unique<osd::gpu_dirty_region>(GPU_TEXTURE_WIDTH, GPU_TEXTURE_HEIGHT);
	m_gpu_fb_flags.assign(size_t(GPU_TARGET_WIDTH) * GPU_TARGET_ROWS, 0);
	m_gpu_ready = true;
	gpu_reset();
	osd_printf_info("midzeus: host GPU path on, target %dx%d, %dx supersampling, %d MSAA samples\n",
			GPU_TARGET_WIDTH * m_gpu_scale, GPU_TARGET_ROWS * m_gpu_scale, m_gpu_scale, m_gpu_msaa);
	return true;
}

// Called when the frame buffer the target holds can no longer be trusted: a state load or a reset. The target is cleared at the next
// frame, wave RAM 0 is uploaded again in full, and the frame buffer's words are stamped in full, so the CPU's copy of the frame
// buffer is what the next frame starts from.
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
	if (m_gpu_ready)
	{
		std::fill(m_gpu_fb_flags.begin(), m_gpu_fb_flags.end(), uint8_t(GPU_FB_COLOUR | GPU_FB_DEPTH));
		m_gpu_fb_pending = true;
		m_gpu_fb_row_min = 0;
		m_gpu_fb_row_max = GPU_TARGET_ROWS - 1;
	}
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
	m_gpu_fb_pending = false;
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

// A CPU write to the frame-buffer block at block. mask holds the words of the write (0xb6 bits 20 to 23): words 0 and 1 are the
// colour of the block's two pixels, words 2 and 3 their depth. The write is held until the next flush.
void midzeus_renderer::gpu_mark_frame_buffer(const void *block, uint32_t mask)
{
	if (!m_gpu_ready || !(mask & 0x00f00000))
		return;
	const uint32_t offset = uint32_t(uintptr_t(block) - uintptr_t(m_state.m_waveram[1].get()));
	const int row = int(offset / GPU_ROW_BYTES);
	if (row >= GPU_TARGET_ROWS)
		return;
	uint8_t *const flags = &m_gpu_fb_flags[size_t(row) * GPU_TARGET_WIDTH + 2 * ((offset % GPU_ROW_BYTES) / GPU_BLOCK_BYTES)];
	if (mask & 0x00100000)
		flags[0] |= GPU_FB_COLOUR;
	if (mask & 0x00200000)
		flags[1] |= GPU_FB_COLOUR;
	if (mask & 0x00400000)
		flags[0] |= GPU_FB_DEPTH;
	if (mask & 0x00800000)
		flags[1] |= GPU_FB_DEPTH;

	if (!m_gpu_fb_pending)
	{
		m_gpu_fb_pending = true;
		m_gpu_fb_row_min = row;
		m_gpu_fb_row_max = row;
	}
	else
	{
		m_gpu_fb_row_min = std::min(m_gpu_fb_row_min, row);
		m_gpu_fb_row_max = std::max(m_gpu_fb_row_max, row);
	}
}

// The game flips the render base (0x84) or the display base (0xcc). The queued polygons are sent now, so the queue stays
// short on frames that are not shown; the rest of the frame goes out with screen_update().
void midzeus_renderer::gpu_flip()
{
	if (!m_gpu_ready || (m_gpu_commands.empty() && !m_gpu_fb_pending))
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

// The CPU writes to the frame buffer since the last flush, placed at this point of the command list. One texture update per
// rectangle of blocks copies the frame buffer's words as they stand in the CPU's copy. Then a stamp draw per plane and rectangle
// copies the flagged words of that plane into the target, clipped to the rectangle and writing only that plane. The update covers
// whole blocks, so an unflagged word of a block is copied too, but no stamp reads it.
void midzeus_renderer::gpu_flush_frame_buffer()
{
	if (!m_gpu_fb_pending)
		return;
	m_gpu_fb_pending = false;

	const uint8_t *const ram = (const uint8_t *)m_state.m_waveram[1].get();
	const int row_min = m_gpu_fb_row_min;
	const int row_max = m_gpu_fb_row_max;

	gpu_flag_rects(m_gpu_fb_flags, row_min, row_max, GPU_FB_COLOUR | GPU_FB_DEPTH,
		[this, ram](int x0, int x1, int y0, int y1)
		{
			const int bx0 = x0 / 2;
			const int bx1 = (x1 + 1) / 2;
			const int w = 4 * (bx1 - bx0);
			const int h = y1 - y0;
			auto &blob = m_gpu_blobs.emplace_back(size_t(w) * h * 2);
			for (int row = 0; row < h; row++)
				memcpy(&blob[size_t(row) * w * 2], ram + size_t(y0 + row) * GPU_ROW_BYTES + size_t(bx0) * GPU_BLOCK_BYTES, size_t(w) * 2);

			osd::gpu_texture_update update;
			update.texture = m_gpu_fb_texture;
			update.x = 4 * bx0;
			update.y = y0;
			update.w = w;
			update.h = h;
			update.data = blob.data();
			m_gpu_commands.push_back(osd::gpu_command::make_update(update));
		});

	for (uint32_t plane = GPU_STAMP_COLOUR; plane <= GPU_STAMP_DEPTH; plane++)
	{
		const uint8_t mask = (plane == GPU_STAMP_COLOUR) ? GPU_FB_COLOUR : GPU_FB_DEPTH;
		gpu_flag_rects(m_gpu_fb_flags, row_min, row_max, mask,
			[this, plane](int x0, int x1, int y0, int y1)
			{
				// the stamp reads its scale from its record (its other words are unused): one native pixel is scale target pixels
				const uint32_t prim = m_gpu_state.size() / MIDZEUS_SHADER_STATE_WORDS;
				m_gpu_state.resize(m_gpu_state.size() + MIDZEUS_SHADER_STATE_WORDS, 0);
				m_gpu_state[prim * MIDZEUS_SHADER_STATE_WORDS + SW_SCALE] = uint32_t(m_gpu_scale);

				const float scale = float(m_gpu_scale);
				auto corner = [prim, scale](float x, float y)
				{
					gpu_vertex v = {};
					v.x = x * scale;
					v.y = y * scale;
					v.z = 0.0f;
					v.w = 1.0f;
					v.prim = prim;
					return v;
				};
				const gpu_vertex a = corner(float(x0), float(y0));
				const gpu_vertex b = corner(float(x1), float(y0));
				const gpu_vertex c = corner(float(x1), float(y1));
				const gpu_vertex d = corner(float(x0), float(y1));
				const uint32_t first = m_gpu_vertices.size();
				m_gpu_vertices.insert(m_gpu_vertices.end(), { a, b, c, a, c, d });

				m_gpu_commands.push_back(osd::gpu_command::make_clip(x0 * m_gpu_scale, y0 * m_gpu_scale, (x1 - x0) * m_gpu_scale, (y1 - y0) * m_gpu_scale));
				osd::gpu_command draw = osd::gpu_command::make_draw(m_gpu_stamp_module, first, 6);
				draw.bindings[0] = m_gpu_fb_texture;
				draw.variant = plane;
				draw.depth_func = osd::gpu_depth_func::ALWAYS;
				draw.depth_write = (plane == GPU_STAMP_DEPTH);
				draw.color_write = (plane == GPU_STAMP_COLOUR);
				m_gpu_commands.push_back(draw);

				// the next polygon sets its own clip
				m_gpu_clip_row = -1;
			});
	}

	std::fill(m_gpu_fb_flags.begin() + size_t(row_min) * GPU_TARGET_WIDTH, m_gpu_fb_flags.begin() + size_t(row_max + 1) * GPU_TARGET_WIDTH, 0);
}

// Sets the scissor to the clip rectangle of a page. The clip is in page coordinates, so the page's first row is added.
void midzeus_renderer::gpu_set_clip(int row, const rectangle &clip)
{
	if (row == m_gpu_clip_row && clip.min_x == m_gpu_clip.min_x && clip.max_x == m_gpu_clip.max_x && clip.min_y == m_gpu_clip.min_y && clip.max_y == m_gpu_clip.max_y)
		return;
	m_gpu_clip_row = row;
	m_gpu_clip = clip;
	m_gpu_commands.push_back(osd::gpu_command::make_clip(clip.min_x * m_gpu_scale, (row + clip.min_y) * m_gpu_scale, clip.width() * m_gpu_scale, clip.height() * m_gpu_scale));
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

	// The blend modes of render_poly(). The shader computes the source term of each mode, and the fixed-function blend combines it
	// with the pixel the target holds. OPAQUE1 and OPAQUE4 (and unknown values) write the colour as it is; OPAQUE2 and OPAQUE3 scale
	// it by the intensity, which is always 0xffff, so by 255/256; OPAQUE5 scales it by srca/256. ADD1 and ADD2 add their term to the
	// destination, and MUL1 multiplies it by the destination. The chip reads the destination only when bit 23 is set (all three
	// modes have it), so without it the destination counts as 0: ADD1 and ADD2 then keep the term, and MUL1 writes 0.
	uint32_t term = GPU_BLEND_PLAIN;
	osd::gpu_blend_state blend_state;
	if (!fill)
	{
		switch (state.blend)
		{
			case BLEND_OPAQUE2:
			case BLEND_OPAQUE3:
				term = GPU_BLEND_OPAQUE_FACTOR;
				break;

			case BLEND_OPAQUE5:
				term = GPU_BLEND_SRCA_FACTOR;
				break;

			case BLEND_ADD1:
				term = GPU_BLEND_SRCA_FACTOR;
				if (state.blend & GPU_BLEND_DST_BIT)
					blend_state = gpu_blend(osd::gpu_blend_factor::ONE, osd::gpu_blend_factor::ONE);
				break;

			case BLEND_ADD2:
				// the destination factor 2 * dsta / 256 is the blend constant; dsta above 128 gives a factor above 1 that the
				// blend clamps, which the chip does not (not seen in attract)
				term = GPU_BLEND_SRCA_FACTOR;
				if (state.blend & GPU_BLEND_DST_BIT)
				{
					const uint32_t dsta = (state.alpha >> 8) & 0xff;
					blend_state = gpu_blend(osd::gpu_blend_factor::ONE, osd::gpu_blend_factor::CONSTANT_ALPHA);
					blend_state.constant_alpha = float(std::min<uint32_t>(dsta << 1, 256)) / 256.0f;
				}
				break;

			case BLEND_MUL1:
				// the source factor is 2 * srca / 256 and the destination factor is dst / 256 (the blend uses dst / 255); a term
				// above 255 is clamped, which the chip does not (not seen in attract)
				term = GPU_BLEND_MUL_FACTOR;
				blend_state = gpu_blend((state.blend & GPU_BLEND_DST_BIT) ? osd::gpu_blend_factor::DST_COLOR : osd::gpu_blend_factor::ZERO, osd::gpu_blend_factor::ZERO);
				break;

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
	words[SW_BLEND] = term;
	words[SW_SRCA] = state.alpha & 0xff;
	words[SW_ZOFFSET] = uint32_t(int32_t(state.zoffset));
	words[SW_SCALE] = uint32_t(m_gpu_scale);

	// the fill is an unconditional write of colour and depth, as render_poly_solid_fixedz() does it
	const bool depth_test = !fill && state.depth_test_enable;
	const bool depth_write = fill || state.depth_write_enable;
	words[SW_DEPTH_TEST] = depth_test ? 1 : 0;

	const int row = gpu_page_row(state.renderbase);
	gpu_flush_frame_buffer();
	gpu_flush_textures();
	gpu_set_clip(row, clip);

	// texture coordinates in texel units; the depth is the chip's 16.16 z, which the shader reduces to the integer depth
	const float scale = float(m_gpu_scale);
	const uint32_t prim = m_gpu_state.size() / SW_COUNT;
	gpu_vertex vv[6];
	for (uint32_t i = 0; i < numverts; i++)
	{
		vv[i].x = vert[i].x * scale;
		vv[i].y = (vert[i].y + row) * scale;
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

	// consecutive polygons with the same depth and blend state, and nothing between them, share one draw
	if (!m_gpu_commands.empty())
	{
		osd::gpu_command &last = m_gpu_commands.back();
		if (last.type == osd::gpu_command_type::DRAW && last.module == m_gpu_module && last.first_vertex + last.vertex_count == first && last.depth_func == func && last.depth_write == depth_write && gpu_same_blend(last.blend, blend_state))
		{
			last.vertex_count += count;
			return;
		}
	}
	osd::gpu_command draw = osd::gpu_command::make_draw(m_gpu_module, first, count);
	draw.bindings[0] = m_gpu_texture;
	draw.depth_func = func;
	draw.depth_write = depth_write;
	draw.blend = blend_state;
	m_gpu_commands.push_back(draw);
}

// Sends the queued frame. A readback is written by this call; flush sends the commands without one, to keep the queue short.
bool midzeus_renderer::gpu_submit(const osd::gpu_readback_rect *readback, bool flush)
{
	gpu_flush_frame_buffer();

	osd::gpu_frame frame;
	frame.target.width = GPU_TARGET_WIDTH * m_gpu_scale;
	frame.target.height = GPU_TARGET_ROWS * m_gpu_scale;
	frame.target.persistent = true;
	frame.target.depth = osd::gpu_depth_mode::REAL;
	frame.target.msaa_samples = m_gpu_msaa;
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
		readback.x = (x0 - vis.min_x) * m_gpu_scale;
		readback.y = (display + y0) * m_gpu_scale;
		readback.w = (x1 - x0 + 1) * m_gpu_scale;
		readback.h = (y1 - y0 + 1) * m_gpu_scale;
		readback.downsample = m_gpu_scale;
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
