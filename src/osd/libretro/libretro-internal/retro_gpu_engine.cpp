// license:BSD-3-Clause
// copyright-holders:TrickoUK
/***************************************************************************

    retro_gpu_engine.cpp

    See retro_gpu_engine.h and interface/gpuengine.h.

***************************************************************************/
#include "retro_gpu_engine.h"
#include "retro_gl_loader.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace retro_gl;

#ifndef GL_FUNC_SUBTRACT
#define GL_FUNC_SUBTRACT 0x800A
#endif

namespace {

using clock_type = std::chrono::steady_clock;

double elapsed_ms(clock_type::time_point from)
{
	return std::chrono::duration<double, std::milli>(clock_type::now() - from).count();
}

// Makes the engine's EGL context current for the lifetime of the object and
// restores whatever the thread had current beforehand.
struct scoped_context
{
	EGLDisplay saved_display;
	EGLSurface saved_draw;
	EGLSurface saved_read;
	EGLContext saved_context;
	bool active;

	scoped_context(EGLDisplay display, EGLSurface surface, EGLContext context)
	{
		saved_display = g_egl.GetCurrentDisplay();
		saved_draw = g_egl.GetCurrentSurface(EGL_DRAW_);
		saved_read = g_egl.GetCurrentSurface(EGL_READ_);
		saved_context = g_egl.GetCurrentContext();
		active = g_egl.MakeCurrent(display, surface, surface, context);
		if (!active)
			fprintf(stderr, "[retro_gpu_engine] eglMakeCurrent failed: 0x%x\n", g_egl.GetError());
	}

	~scoped_context()
	{
		if (active)
			g_egl.MakeCurrent(saved_display, saved_draw, saved_read, saved_context);
	}

	scoped_context(const scoped_context &) = delete;
	scoped_context &operator=(const scoped_context &) = delete;
};

struct format_info
{
	GLint internal_format;
	GLenum format;
	GLenum type;
	int bytes;
};

format_info get_format_info(osd::gpu_texture_format f)
{
	switch (f)
	{
	case osd::gpu_texture_format::R8UI:     return { GL_R8UI, GL_RED_INTEGER, GL_UNSIGNED_BYTE, 1 };
	case osd::gpu_texture_format::R16UI:    return { GL_R16UI, GL_RED_INTEGER, GL_UNSIGNED_SHORT, 2 };
	case osd::gpu_texture_format::R32UI:    return { GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, 4 };
	case osd::gpu_texture_format::RG32UI:   return { GL_RG32UI, GL_RG_INTEGER, GL_UNSIGNED_INT, 8 };
	case osd::gpu_texture_format::RGBA32I:  return { GL_RGBA32I, GL_RGBA_INTEGER, GL_INT, 16 };
	case osd::gpu_texture_format::RGBA8:    break;
	}
	return { GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 4 };
}

const char *sampler_type(osd::gpu_texture_format f)
{
	switch (f)
	{
	case osd::gpu_texture_format::RGBA32I:  return "isampler2D";
	case osd::gpu_texture_format::RGBA8:    return "sampler2D";
	default:                                return "usampler2D";
	}
}

GLenum depth_func_enum(osd::gpu_depth_func f)
{
	switch (f)
	{
	case osd::gpu_depth_func::NEVER:    return GL_NEVER;
	case osd::gpu_depth_func::LESS:     return GL_LESS;
	case osd::gpu_depth_func::LEQUAL:   return GL_LEQUAL;
	case osd::gpu_depth_func::EQUAL:    return GL_EQUAL;
	case osd::gpu_depth_func::GEQUAL:   return GL_GEQUAL;
	case osd::gpu_depth_func::GREATER:  return GL_GREATER;
	case osd::gpu_depth_func::NOTEQUAL: return GL_NOTEQUAL;
	case osd::gpu_depth_func::ALWAYS:   break;
	}
	return GL_ALWAYS;
}

GLenum blend_factor_enum(osd::gpu_blend_factor f)
{
	switch (f)
	{
	case osd::gpu_blend_factor::ZERO:                       return GL_ZERO;
	case osd::gpu_blend_factor::ONE:                        return GL_ONE;
	case osd::gpu_blend_factor::SRC_ALPHA:                  return GL_SRC_ALPHA;
	case osd::gpu_blend_factor::ONE_MINUS_SRC_ALPHA:        return GL_ONE_MINUS_SRC_ALPHA;
	case osd::gpu_blend_factor::CONSTANT_ALPHA:             return GL_CONSTANT_ALPHA;
	case osd::gpu_blend_factor::ONE_MINUS_CONSTANT_ALPHA:   return GL_ONE_MINUS_CONSTANT_ALPHA;
	case osd::gpu_blend_factor::SRC_COLOR:                  return GL_SRC_COLOR;
	case osd::gpu_blend_factor::ONE_MINUS_SRC_COLOR:        return GL_ONE_MINUS_SRC_COLOR;
	case osd::gpu_blend_factor::DST_COLOR:                  return GL_DST_COLOR;
	case osd::gpu_blend_factor::ONE_MINUS_DST_COLOR:        return GL_ONE_MINUS_DST_COLOR;
	case osd::gpu_blend_factor::SRC_ALPHA_SATURATE:         break;
	}
	return GL_SRC_ALPHA_SATURATE;
}

// flat varyings have a single value and take no centroid qualifier
std::string interpolation_qualifier(osd::gpu_varying_mode m, bool centroid)
{
	std::string q;
	switch (m)
	{
	case osd::gpu_varying_mode::NOPERSPECTIVE:  q = "noperspective "; break;
	case osd::gpu_varying_mode::FLAT:           return "flat ";
	case osd::gpu_varying_mode::PERSPECTIVE:    break;
	}
	if (centroid)
		q += "centroid ";
	return q;
}

constexpr int STATE_TEX_WIDTH = 2048;

bool valid_identifier(const char *s)
{
	if (!s || !*s)
		return false;
	for (const char *p = s; *p; p++)
	{
		const bool alpha = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || *p == '_';
		const bool digit = *p >= '0' && *p <= '9';
		if (!alpha && !(digit && p != s))
			return false;
	}
	return true;
}

bool reserved_name(const char *s)
{
	return !strncmp(s, "u_", 2) || !strncmp(s, "g_", 2) || !strncmp(s, "o_", 2) || !strncmp(s, "a_", 2) || !strncmp(s, "gl_", 3)
			|| !strcmp(s, "state") || !strcmp(s, "frag_pos") || !strcmp(s, "target_size") || !strcmp(s, "shade") || !strcmp(s, "main");
}

} // anonymous namespace

retro_gpu_engine::retro_gpu_engine()
{
	m_valid = init_context();
}

retro_gpu_engine::~retro_gpu_engine()
{
	shutdown();
}

bool retro_gpu_engine::init_context()
{
	setvbuf(stderr, nullptr, _IONBF, 0);

	if (!ensure_dispatch_loaded())
		return false;

	// The surfaceless platform has no windowing-system involvement, so it
	// cannot interfere with the frontend's own EGL setup.
	m_display = g_egl.GetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, nullptr, nullptr);
	if (!m_display)
		return false;

	EGLint major = 0, minor = 0;
	if (!g_egl.Initialize(m_display, &major, &minor))
		return false;
	if (!g_egl.BindAPI(EGL_OPENGL_API_))
		return false;

	EGLint cfg_attribs[] = {
		EGL_SURFACE_TYPE_, EGL_PBUFFER_BIT_,
		EGL_RENDERABLE_TYPE_, EGL_OPENGL_BIT_,
		EGL_RED_SIZE_, 8, EGL_GREEN_SIZE_, 8, EGL_BLUE_SIZE_, 8, EGL_ALPHA_SIZE_, 8,
		EGL_NONE_
	};
	EGLConfig cfg;
	EGLint num_cfg = 0;
	if (!g_egl.ChooseConfig(m_display, cfg_attribs, &cfg, 1, &num_cfg) || num_cfg < 1)
		return false;

	EGLint pbuf_attribs[] = { EGL_WIDTH_, 16, EGL_HEIGHT_, 16, EGL_NONE_ };
	m_surface = g_egl.CreatePbufferSurface(m_display, cfg, pbuf_attribs);
	if (!m_surface)
		return false;

	EGLint ctx_attribs[] = {
		EGL_CONTEXT_MAJOR_VERSION_, 3, EGL_CONTEXT_MINOR_VERSION_, 3,
		EGL_CONTEXT_OPENGL_PROFILE_MASK_, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_,
		EGL_NONE_
	};
	m_context = g_egl.CreateContext(m_display, cfg, EGL_NO_CONTEXT_, ctx_attribs);
	if (!m_context)
		return false;

	scoped_context ctx(m_display, m_surface, m_context);
	if (!ctx.active)
		return false;

	if (!load_gl_dispatch())
		return false;

	m_renderer = std::string((const char *)g_gl.GetString(GL_RENDERER));
	m_caps.renderer = m_renderer.c_str();
	const char *version = (const char *)g_gl.GetString(GL_VERSION);
	if (version)
		sscanf(version, "%d.%d", &m_caps.gl_major, &m_caps.gl_minor);
	GLint value = 0;
	g_gl.GetIntegerv(GL_MAX_TEXTURE_SIZE, &value);
	m_caps.max_texture_size = value;
	value = 0;
	g_gl.GetIntegerv(GL_MAX_SAMPLES, &value);
	m_caps.max_samples = value;
	value = 0;
	g_gl.GetIntegerv(GL_MAX_COLOR_TEXTURE_SAMPLES, &value);
	m_max_texture_samples = value;

	g_gl.GenVertexArrays(1, &m_vao);
	g_gl.GenBuffers(1, &m_vbo);

	m_timers = (std::getenv("MAME_GPU_PROF") != nullptr) || (std::getenv("MAME_VOODOO_GPU_PROF") != nullptr);
	if (m_timers)
		g_gl.GenQueries(1, &m_query);

	fprintf(stderr, "[retro_gpu_engine] initialized: %s / %s / %s\n",
			(const char *)g_gl.GetString(GL_VENDOR), m_renderer.c_str(), version ? version : "?");
	return true;
}

void retro_gpu_engine::shutdown()
{
	if (!m_display || !g_dispatch_loaded)
		return;

	{
		scoped_context ctx(m_display, m_surface, m_context);
		if (ctx.active && m_valid)
		{
			release_target();
			flush_deleted();
			for (auto &t : m_textures)
				if (t.second.gl_id)
					g_gl.DeleteTextures(1, &t.second.gl_id);
			for (auto &p : m_programs)
				if (p.second.gl_id)
					g_gl.DeleteProgram(p.second.gl_id);
			if (m_state_tex)
				g_gl.DeleteTextures(1, &m_state_tex);
			if (m_vbo)
				g_gl.DeleteBuffers(1, &m_vbo);
			if (m_vao)
				g_gl.DeleteVertexArrays(1, &m_vao);
			if (m_query)
				g_gl.DeleteQueries(1, &m_query);
			drop_pending();
			if (m_pbo)
				g_gl.DeleteBuffers(1, &m_pbo);
			for (output_program &op : m_op)
				if (op.id)
					g_gl.DeleteProgram(op.id);
			if (m_op_vao)
				g_gl.DeleteVertexArrays(1, &m_op_vao);
			if (m_op_src_fbo)
				g_gl.DeleteFramebuffers(1, &m_op_src_fbo);
			if (m_op_src_tex)
				g_gl.DeleteTextures(1, &m_op_src_tex);
			if (m_op_dst_fbo)
				g_gl.DeleteFramebuffers(1, &m_op_dst_fbo);
			if (m_op_dst_tex)
				g_gl.DeleteTextures(1, &m_op_dst_tex);
		}
	}
	m_textures.clear();
	m_programs.clear();
	m_state_tex = 0;

	if (m_context)
		g_egl.DestroyContext(m_display, m_context);
	if (m_surface)
		g_egl.DestroySurface(m_display, m_surface);
	g_egl.Terminate(m_display);
	m_context = nullptr;
	m_surface = nullptr;
	m_display = nullptr;
	m_valid = false;
}

void retro_gpu_engine::log_problem(const char *what, const char *detail)
{
	// a broken adapter would otherwise print every frame
	if (m_problem_count < 20)
		fprintf(stderr, "[retro_gpu_engine] %s: %s\n", what, detail);
	else if (m_problem_count == 20)
		fprintf(stderr, "[retro_gpu_engine] further problems are not logged\n");
	m_problem_count++;
}


//**************************************************************************
//  Textures
//**************************************************************************

osd::gpu_handle retro_gpu_engine::create_texture(const osd::gpu_texture_desc &desc)
{
	if (!m_valid || desc.width <= 0 || desc.height <= 0
			|| desc.width > m_caps.max_texture_size || desc.height > m_caps.max_texture_size)
		return osd::GPU_NULL_HANDLE;

	texture t;
	t.desc = desc;
	t.bytes = uint64_t(desc.width) * desc.height * get_format_info(desc.format).bytes;
	if (m_texture_bytes + t.bytes > m_texture_budget)
	{
		log_problem("create_texture", "texture memory budget exceeded");
		return osd::GPU_NULL_HANDLE;
	}

	m_texture_bytes += t.bytes;
	const osd::gpu_handle handle = m_next_handle++;
	m_textures.emplace(handle, t);
	return handle;
}

void retro_gpu_engine::destroy_texture(osd::gpu_handle handle)
{
	const auto it = m_textures.find(handle);
	if (it == m_textures.end())
		return;
	if (it->second.gl_id)
		m_deleted_textures.push_back(it->second.gl_id);
	m_texture_bytes -= it->second.bytes;
	m_textures.erase(it);
}

bool retro_gpu_engine::ensure_texture(texture &t)
{
	if (t.gl_id)
		return true;

	const format_info fi = get_format_info(t.desc.format);
	g_gl.GenTextures(1, &t.gl_id);
	g_gl.ActiveTexture(GL_TEXTURE0);
	g_gl.BindTexture(GL_TEXTURE_2D, t.gl_id);
	g_gl.TexImage2D(GL_TEXTURE_2D, 0, fi.internal_format, t.desc.width, t.desc.height, 0, fi.format, fi.type, nullptr);
	const GLint filter = (t.desc.format == osd::gpu_texture_format::RGBA8 && t.desc.filter_linear) ? GL_LINEAR : GL_NEAREST;
	g_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
	g_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
	g_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	g_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	return true;
}

bool retro_gpu_engine::validate_update(const osd::gpu_texture_update &u)
{
	const auto it = m_textures.find(u.texture);
	if (it == m_textures.end())
	{
		log_problem("texture update", "unknown texture handle");
		return false;
	}
	const texture &t = it->second;
	if (!u.data || u.w <= 0 || u.h <= 0 || u.x < 0 || u.y < 0 || u.x + u.w > t.desc.width || u.y + u.h > t.desc.height)
	{
		log_problem("texture update", "rectangle outside the texture or no data");
		return false;
	}
	const int bpp = get_format_info(t.desc.format).bytes;
	if (u.pitch_bytes != 0 && (u.pitch_bytes % bpp != 0 || u.pitch_bytes < u.w * bpp))
	{
		log_problem("texture update", "pitch is not a whole number of texels or is smaller than a row");
		return false;
	}
	return true;
}

bool retro_gpu_engine::apply_update(const osd::gpu_texture_update &u)
{
	texture &t = m_textures.find(u.texture)->second;
	ensure_texture(t);

	const format_info fi = get_format_info(t.desc.format);
	g_gl.ActiveTexture(GL_TEXTURE0);
	g_gl.BindTexture(GL_TEXTURE_2D, t.gl_id);
	g_gl.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
	g_gl.PixelStorei(GL_UNPACK_ROW_LENGTH, u.pitch_bytes ? u.pitch_bytes / fi.bytes : 0);
	g_gl.TexSubImage2D(GL_TEXTURE_2D, 0, u.x, u.y, u.w, u.h, fi.format, fi.type, u.data);
	g_gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	m_stats.uploaded_bytes += uint64_t(u.w) * u.h * fi.bytes;
	return true;
}

void retro_gpu_engine::flush_deleted()
{
	for (uint32_t id : m_deleted_textures)
		g_gl.DeleteTextures(1, &id);
	m_deleted_textures.clear();
	for (uint32_t id : m_deleted_programs)
		g_gl.DeleteProgram(id);
	m_deleted_programs.clear();
}


//**************************************************************************
//  Shader modules and programs
//**************************************************************************

osd::gpu_handle retro_gpu_engine::create_shader_module(const osd::gpu_shader_module &desc)
{
	if (!m_valid || !desc.fragment_source || desc.varying_count < 0 || desc.varying_count > osd::GPU_MAX_VARYINGS
			|| desc.binding_count < 0 || desc.binding_count > osd::GPU_MAX_BINDINGS || desc.state_stride < 0)
		return osd::GPU_NULL_HANDLE;

	for (int i = 0; i < desc.binding_count; i++)
	{
		if (!valid_identifier(desc.binding_name[i]) || reserved_name(desc.binding_name[i]))
		{
			log_problem("create_shader_module", "invalid or reserved binding name");
			return osd::GPU_NULL_HANDLE;
		}
		for (int j = 0; j < i; j++)
			if (!strcmp(desc.binding_name[i], desc.binding_name[j]))
			{
				log_problem("create_shader_module", "duplicate binding name");
				return osd::GPU_NULL_HANDLE;
			}
	}

	module m;
	m.name = desc.name ? desc.name : "";
	m.source = desc.fragment_source;
	m.centroid_mask = desc.centroid_mask;
	m.varying_count = desc.varying_count;
	std::copy(std::begin(desc.varying_mode), std::end(desc.varying_mode), std::begin(m.varying_mode));
	m.state_stride = desc.state_stride;
	m.writes_depth = desc.writes_depth;
	m.binding_count = desc.binding_count;
	for (int i = 0; i < desc.binding_count; i++)
	{
		m.binding_name[i] = desc.binding_name[i];
		m.binding_format[i] = desc.binding_format[i];
	}

	const osd::gpu_handle handle = m_next_handle++;
	m_modules.emplace(handle, std::move(m));
	return handle;
}

void retro_gpu_engine::destroy_shader_module(osd::gpu_handle handle)
{
	if (m_modules.erase(handle) == 0)
		return;
	for (auto it = m_programs.begin(); it != m_programs.end(); )
	{
		if (it->first.first == handle)
		{
			if (it->second.gl_id)
				m_deleted_programs.push_back(it->second.gl_id);
			it = m_programs.erase(it);
		}
		else
			++it;
	}
}

const retro_gpu_engine::program *retro_gpu_engine::get_program(osd::gpu_handle module_handle, uint32_t variant)
{
	const auto key = std::make_pair(module_handle, variant);
	const auto found = m_programs.find(key);
	if (found != m_programs.end())
		return &found->second;

	const module &m = m_modules.find(module_handle)->second;
	const auto start = clock_type::now();

	// vertex shader: position, primitive id and the varyings packed in vec4 groups
	const int groups = (m.varying_count + 3) / 4;
	std::string vs = "#version 330 core\n";
	vs += "layout(location = 0) in vec4 a_pos;\n";
	vs += "layout(location = 1) in uint a_prim;\n";
	for (int g = 0; g < groups; g++)
		vs += "layout(location = " + std::to_string(2 + g) + ") in vec4 a_var" + std::to_string(g) + ";\n";
	vs += "uniform vec2 u_target_size;\nuniform float u_order_scale;\nuniform int u_depth_order;\n";
	vs += "flat out uint g_prim;\n";
	for (int i = 0; i < m.varying_count; i++)
		vs += interpolation_qualifier(m.varying_mode[i], (((m.centroid_mask >> i) & 1) != 0)) + "out float v" + std::to_string(i) + ";\n";
	vs += "void main()\n{\n";
	// rows are used top-down: target y = 0 is framebuffer row 0
	vs += "\tvec2 ndc = a_pos.xy / u_target_size * 2.0 - 1.0;\n";
	vs += "\tfloat z = (u_depth_order != 0) ? (float(a_prim) + 1.0) * u_order_scale : a_pos.z;\n";
	vs += "\tfloat w = a_pos.w;\n";
	vs += "\tgl_Position = vec4(ndc * w, (z * 2.0 - 1.0) * w, w);\n";
	vs += "\tg_prim = a_prim;\n";
	static const char *const swizzle = "xyzw";
	for (int i = 0; i < m.varying_count; i++)
		vs += "\tv" + std::to_string(i) + " = a_var" + std::to_string(i / 4) + "." + swizzle[i % 4] + ";\n";
	vs += "}\n";

	// fragment shader: prelude plus the module source
	std::string fs = "#version 330 core\n";
	fs += "#define VARIANT " + std::to_string(variant) + "\n";
	fs += "flat in uint g_prim;\n";
	for (int i = 0; i < m.varying_count; i++)
		fs += interpolation_qualifier(m.varying_mode[i], (((m.centroid_mask >> i) & 1) != 0)) + "in float v" + std::to_string(i) + ";\n";
	fs += "out vec4 o_color;\n";
	fs += "uniform vec2 u_target_size;\nuniform int u_rec_texels;\nuniform isampler2D u_state_tex;\n";
	fs += "int state(int k)\n{\n";
	fs += "\tint idx = int(g_prim) * u_rec_texels + (k >> 2);\n";
	fs += "\tivec4 t = texelFetch(u_state_tex, ivec2(idx & 2047, idx >> 11), 0);\n";
	fs += "\tint c = k & 3;\n";
	fs += "\treturn (c == 0) ? t.x : (c == 1) ? t.y : (c == 2) ? t.z : t.w;\n}\n";
	fs += "vec2 frag_pos()\n{\n\treturn gl_FragCoord.xy;\n}\n";
	fs += "ivec2 target_size()\n{\n\treturn ivec2(u_target_size);\n}\n";
	for (int i = 0; i < m.binding_count; i++)
	{
		fs += std::string("uniform ") + sampler_type(m.binding_format[i]) + " " + m.binding_name[i] + ";\n";
		fs += "ivec2 " + m.binding_name[i] + "_size()\n{\n\treturn textureSize(" + m.binding_name[i] + ", 0);\n}\n";
	}
	if (m.writes_depth)
		fs += "float frag_depth = 0.0;\n";
	fs += m.source + "\n";
	fs += std::string("void main()\n{\n\tvec4 c;\n\tif (!shade(c))\n\t\tdiscard;\n\to_color = c;\n") + (m.writes_depth ? "\tgl_FragDepth = frag_depth;\n" : "") + "}\n";

	program p;
	GLuint shaders[2] = { 0, 0 };
	const char *sources[2] = { vs.c_str(), fs.c_str() };
	const GLenum types[2] = { GL_VERTEX_SHADER, GL_FRAGMENT_SHADER };
	bool ok = true;
	for (int i = 0; i < 2 && ok; i++)
	{
		shaders[i] = g_gl.CreateShader(types[i]);
		g_gl.ShaderSource(shaders[i], 1, &sources[i], nullptr);
		g_gl.CompileShader(shaders[i]);
		GLint status = 0;
		g_gl.GetShaderiv(shaders[i], GL_COMPILE_STATUS, &status);
		if (!status)
		{
			char log[4096];
			g_gl.GetShaderInfoLog(shaders[i], sizeof(log), nullptr, log);
			fprintf(stderr, "[retro_gpu_engine] module '%s' variant %u: %s shader failed to compile:\n%s\n",
					m.name.c_str(), variant, i ? "fragment" : "vertex", log);
			ok = false;
		}
	}

	if (ok)
	{
		p.gl_id = g_gl.CreateProgram();
		g_gl.AttachShader(p.gl_id, shaders[0]);
		g_gl.AttachShader(p.gl_id, shaders[1]);
		g_gl.LinkProgram(p.gl_id);
		GLint status = 0;
		g_gl.GetProgramiv(p.gl_id, GL_LINK_STATUS, &status);
		if (!status)
		{
			char log[4096];
			g_gl.GetProgramInfoLog(p.gl_id, sizeof(log), nullptr, log);
			fprintf(stderr, "[retro_gpu_engine] module '%s' variant %u: link failed:\n%s\n", m.name.c_str(), variant, log);
			g_gl.DeleteProgram(p.gl_id);
			p.gl_id = 0;
			ok = false;
		}
	}
	for (GLuint s : shaders)
		if (s)
			g_gl.DeleteShader(s);

	if (!ok)
	{
		// the module is not retried: a failed program latches the engine so the device returns to software
		m_failed = true;
		return nullptr;
	}

	g_gl.UseProgram(p.gl_id);
	p.u_target_size = g_gl.GetUniformLocation(p.gl_id, "u_target_size");
	p.u_order_scale = g_gl.GetUniformLocation(p.gl_id, "u_order_scale");
	p.u_depth_order = g_gl.GetUniformLocation(p.gl_id, "u_depth_order");
	p.u_rec_texels = g_gl.GetUniformLocation(p.gl_id, "u_rec_texels");
	g_gl.Uniform1i(g_gl.GetUniformLocation(p.gl_id, "u_state_tex"), 0);
	for (int i = 0; i < m.binding_count; i++)
		g_gl.Uniform1i(g_gl.GetUniformLocation(p.gl_id, m.binding_name[i].c_str()), 1 + i);

	m_stats.program_compiles++;
	fprintf(stderr, "[retro_gpu_engine] module '%s' variant %u built in %.1f ms\n", m.name.c_str(), variant, elapsed_ms(start));
	return &m_programs.emplace(key, p).first->second;
}


//**************************************************************************
//  Render target
//**************************************************************************

void retro_gpu_engine::release_target()
{
	if (m_fbo)
		g_gl.DeleteFramebuffers(1, &m_fbo);
	if (m_color_rb)
		g_gl.DeleteRenderbuffers(1, &m_color_rb);
	if (m_depth_rb)
		g_gl.DeleteRenderbuffers(1, &m_depth_rb);
	if (m_resolve_fbo)
		g_gl.DeleteFramebuffers(1, &m_resolve_fbo);
	if (m_resolve_tex)
		g_gl.DeleteTextures(1, &m_resolve_tex);
	if (m_color_tex)
		g_gl.DeleteTextures(1, &m_color_tex);
	m_fbo = m_color_rb = m_depth_rb = m_resolve_fbo = m_resolve_tex = m_color_tex = 0;
	m_target_valid = false;
}

// Reduces the requested sample count to what the GPU supports and to the memory budget for the target (colour and
// depth samples plus the single-sample resolve texture). One sample is not multisampling, so it becomes none.
void retro_gpu_engine::fit_msaa(osd::gpu_target_desc &target)
{
	const int requested = target.msaa_samples;
	int limit = m_caps.max_samples;
	if (target.msaa_per_sample)
		limit = std::min(limit, m_max_texture_samples);
	target.msaa_samples = std::clamp(target.msaa_samples, 0, limit);

	const uint64_t pixels = uint64_t(std::max(target.width, 0)) * std::max(target.height, 0);
	const uint64_t sample_bytes = (target.depth != osd::gpu_depth_mode::NONE) ? 8 : 4;
	while (target.msaa_samples >= 2 && pixels * target.msaa_samples * sample_bytes + pixels * 4 > m_msaa_budget)
		target.msaa_samples /= 2;
	if (target.msaa_samples < 2)
		target.msaa_samples = 0;

	if (requested != target.msaa_samples && (requested != m_msaa_logged_requested || target.msaa_samples != m_msaa_logged_granted))
	{
		m_msaa_logged_requested = requested;
		m_msaa_logged_granted = target.msaa_samples;
		fprintf(stderr, "[retro_gpu_engine] %dx MSAA on a %dx%d target reduced to %d samples (GPU limit %d, memory budget %llu MB)\n",
				requested, target.width, target.height, target.msaa_samples, limit, (unsigned long long)(m_msaa_budget >> 20));
	}
}

void retro_gpu_engine::ensure_target(const osd::gpu_target_desc &target)
{
	const bool realloc = !m_target_valid || target.width != m_target.width || target.height != m_target.height
			|| target.depth != m_target.depth || target.msaa_samples != m_target.msaa_samples
			|| target.msaa_per_sample != m_target.msaa_per_sample;

	bool clear_color = !target.persistent;
	bool clear_depth = target.depth == osd::gpu_depth_mode::ORDER || !target.persistent;

	if (realloc)
	{
		release_target();

		g_gl.GenFramebuffers(1, &m_fbo);
		g_gl.BindFramebuffer(GL_FRAMEBUFFER, m_fbo);
		if (target.msaa_samples > 0 && target.msaa_per_sample)
		{
			// a texture, so a read-back can look at the samples one by one
			g_gl.GenTextures(1, &m_color_tex);
			g_gl.BindTexture(GL_TEXTURE_2D_MULTISAMPLE, m_color_tex);
			g_gl.TexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, target.msaa_samples, GL_RGBA8, target.width, target.height, 1);
			g_gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D_MULTISAMPLE, m_color_tex, 0);
		}
		else
		{
			g_gl.GenRenderbuffers(1, &m_color_rb);
			g_gl.BindRenderbuffer(GL_RENDERBUFFER, m_color_rb);
			g_gl.RenderbufferStorageMultisample(GL_RENDERBUFFER, target.msaa_samples, GL_RGBA8, target.width, target.height);
			g_gl.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, m_color_rb);
		}
		if (target.depth != osd::gpu_depth_mode::NONE)
		{
			g_gl.GenRenderbuffers(1, &m_depth_rb);
			g_gl.BindRenderbuffer(GL_RENDERBUFFER, m_depth_rb);
			g_gl.RenderbufferStorageMultisample(GL_RENDERBUFFER, target.msaa_samples, GL_DEPTH_COMPONENT24, target.width, target.height);
			g_gl.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_depth_rb);
		}

		if (g_gl.CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
		{
			log_problem("render target", "framebuffer incomplete");
			release_target();
			m_failed = true;
			return;
		}

		// a multisample target is resolved into a plain texture for reading
		if (target.msaa_samples > 0)
		{
			g_gl.GenFramebuffers(1, &m_resolve_fbo);
			g_gl.BindFramebuffer(GL_FRAMEBUFFER, m_resolve_fbo);
			g_gl.GenTextures(1, &m_resolve_tex);
			g_gl.BindTexture(GL_TEXTURE_2D, m_resolve_tex);
			g_gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, target.width, target.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
			g_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
			g_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
			g_gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_resolve_tex, 0);
		}

		clear_color = true;
		clear_depth = true;
	}
	else if (target.persistent && target.generation != m_target.generation)
	{
		clear_color = true;
		clear_depth = true;
	}

	if (realloc)
	{
		// memory the target asks for: colour and depth for every sample, plus the single-sample resolve texture
		const double pixels = double(target.width) * target.height;
		const double samples = std::max(target.msaa_samples, 1);
		const double depth_bytes = (target.depth != osd::gpu_depth_mode::NONE) ? 4.0 : 0.0;
		const double resolve_bytes = (target.msaa_samples > 0) ? 4.0 : 0.0;
		fprintf(stderr, "[retro_gpu_engine] target %dx%d, %d samples: colour+depth %.1f MB, resolve %.1f MB\n",
				target.width, target.height, target.msaa_samples, pixels * samples * (4.0 + depth_bytes) / 1048576.0, pixels * resolve_bytes / 1048576.0);
	}

	m_target = target;
	m_target_valid = true;

	g_gl.BindFramebuffer(GL_FRAMEBUFFER, m_fbo);
	if (clear_color || clear_depth)
	{
		g_gl.Disable(GL_SCISSOR_TEST);
		g_gl.ColorMask(1, 1, 1, 1);
		g_gl.DepthMask(1);
		GLbitfield bits = 0;
		if (clear_color)
		{
			g_gl.ClearColor(target.clear_color[0], target.clear_color[1], target.clear_color[2], target.clear_color[3]);
			bits |= GL_COLOR_BUFFER_BIT;
		}
		if (clear_depth && target.depth != osd::gpu_depth_mode::NONE)
		{
			g_gl.ClearDepth(1.0);
			bits |= GL_DEPTH_BUFFER_BIT;
		}
		if (bits)
			g_gl.Clear(bits);
	}
}

void retro_gpu_engine::ensure_state_texture(const osd::gpu_frame &frame)
{
	const int rec_texels = std::max(1, (frame.state_stride + 3) / 4);
	const uint64_t total = std::max<uint64_t>(1, uint64_t(frame.prim_count) * rec_texels);
	const int rows = int((total + STATE_TEX_WIDTH - 1) / STATE_TEX_WIDTH);

	if (!m_state_tex)
		g_gl.GenTextures(1, &m_state_tex);
	g_gl.ActiveTexture(GL_TEXTURE0);
	g_gl.BindTexture(GL_TEXTURE_2D, m_state_tex);

	if (rows > m_state_tex_height)
	{
		g_gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32I, STATE_TEX_WIDTH, rows, 0, GL_RGBA_INTEGER, GL_INT, nullptr);
		g_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		g_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		g_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		g_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		m_state_tex_height = rows;
	}

	if (!frame.prim_state || frame.prim_count == 0 || frame.state_stride <= 0)
		return;

	// records are padded to whole texels so one record never straddles a texel
	const size_t words = size_t(rows) * STATE_TEX_WIDTH * 4;
	static thread_local std::vector<int32_t> staging;
	staging.assign(words, 0);
	for (uint32_t i = 0; i < frame.prim_count; i++)
		std::memcpy(&staging[size_t(i) * rec_texels * 4], frame.prim_state + size_t(i) * frame.state_stride, size_t(frame.state_stride) * 4);

	g_gl.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
	g_gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	g_gl.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, STATE_TEX_WIDTH, rows, GL_RGBA_INTEGER, GL_INT, staging.data());
	m_stats.uploaded_bytes += words * 4;
}

void retro_gpu_engine::apply_blend(const osd::gpu_blend_state &blend)
{
	if (!blend.enable)
	{
		g_gl.Disable(GL_BLEND);
		return;
	}
	g_gl.Enable(GL_BLEND);
	switch (blend.op)
	{
	case osd::gpu_blend_op::ADD:                g_gl.BlendEquation(GL_FUNC_ADD); break;
	case osd::gpu_blend_op::SUBTRACT:           g_gl.BlendEquation(GL_FUNC_SUBTRACT); break;
	case osd::gpu_blend_op::REVERSE_SUBTRACT:   g_gl.BlendEquation(GL_FUNC_REVERSE_SUBTRACT); break;
	}
	g_gl.BlendColor(0.0f, 0.0f, 0.0f, blend.constant_alpha);
	g_gl.BlendFunc(blend_factor_enum(blend.src), blend_factor_enum(blend.dst));
}


//**************************************************************************
//  Frames
//**************************************************************************

bool retro_gpu_engine::validate_frame(const osd::gpu_frame &frame)
{
	const osd::gpu_target_desc &tg = frame.target;
	if (tg.width <= 0 || tg.height <= 0 || tg.width > m_caps.max_texture_size || tg.height > m_caps.max_texture_size)
	{
		log_problem("frame", "target size out of range");
		return false;
	}
	if (frame.varyings_per_vertex < 0 || frame.varyings_per_vertex > osd::GPU_MAX_VARYINGS)
	{
		log_problem("frame", "varyings_per_vertex out of range");
		return false;
	}
	if (frame.vertex_count > 0 && !frame.vertices)
	{
		log_problem("frame", "no vertex data");
		return false;
	}
	if (frame.prim_count > 0 && frame.state_stride > 0 && !frame.prim_state)
	{
		log_problem("frame", "no primitive state data");
		return false;
	}

	for (int i = 0; i < frame.update_count; i++)
		if (!validate_update(frame.updates[i]))
			return false;

	for (int i = 0; i < frame.command_count; i++)
	{
		const osd::gpu_command &c = frame.commands[i];
		switch (c.type)
		{
		case osd::gpu_command_type::DRAW:
		{
			const auto mod = m_modules.find(c.module);
			if (mod == m_modules.end())
			{
				log_problem("draw", "unknown shader module");
				return false;
			}
			const module &m = mod->second;
			if (m.varying_count > frame.varyings_per_vertex || m.state_stride > frame.state_stride)
			{
				log_problem("draw", "module needs more varyings or state words than the frame provides");
				return false;
			}
			if (uint64_t(c.first_vertex) + c.vertex_count > frame.vertex_count || c.vertex_count % 3 != 0)
			{
				log_problem("draw", "vertex range outside the frame or not a whole number of triangles");
				return false;
			}
			for (int b = 0; b < m.binding_count; b++)
			{
				const auto tex = m_textures.find(c.bindings[b]);
				if (tex == m_textures.end() || tex->second.desc.format != m.binding_format[b])
				{
					log_problem("draw", "binding missing or has the wrong format");
					return false;
				}
			}
			break;
		}
		case osd::gpu_command_type::COPY_RECT:
			if (c.w <= 0 || c.h <= 0 || c.x < 0 || c.y < 0 || c.dx < 0 || c.dy < 0
					|| c.x + c.w > tg.width || c.y + c.h > tg.height || c.dx + c.w > tg.width || c.dy + c.h > tg.height)
			{
				log_problem("copy", "rectangle outside the target");
				return false;
			}
			break;
		case osd::gpu_command_type::UPDATE_TEXTURE:
			if (!validate_update(c.update))
				return false;
			break;
		case osd::gpu_command_type::READBACK:
			if (!valid_readback(c.readback, tg))
				return false;
			break;
		case osd::gpu_command_type::SET_CLIP:
		case osd::gpu_command_type::CLEAR:
			break;
		}
	}

	for (int i = 0; i < frame.readback_count; i++)
		if (!valid_readback(frame.readbacks[i], tg))
			return false;
	return true;
}

bool retro_gpu_engine::valid_readback(const osd::gpu_readback_rect &r, const osd::gpu_target_desc &tg)
{
	const int ds = std::max(r.downsample, 1);
	const bool rg8 = r.format == osd::gpu_readback_format::RG8;
	if ((rg8 ? (!r.out16 || !r.plain()) : !r.out) || r.w <= 0 || r.h <= 0 || r.x < 0 || r.y < 0 || r.x + r.w > tg.width || r.y + r.h > tg.height
			|| r.w % ds != 0 || r.h % ds != 0 || r.stride < r.w / ds)
	{
		log_problem("readback", "rectangle outside the target or bad buffer");
		return false;
	}
	return true;
}

// Reads the finished timer query, if any. With wait false a result that has not arrived is left for later.
void retro_gpu_engine::poll_gpu_timer(bool wait)
{
	if (!m_query_pending)
		return;
	GLint available = 0;
	g_gl.GetQueryObjectiv(m_query, GL_QUERY_RESULT_AVAILABLE, &available);
	if (!available && !wait)
		return;
	uint64_t ns = 0;
	g_gl.GetQueryObjectui64v(m_query, GL_QUERY_RESULT, &ns);
	m_last_gpu_ms = double(ns) * 1e-6;
	m_query_pending = false;
}

bool retro_gpu_engine::ensure_output_program(bool per_sample)
{
	output_program &op = m_op[per_sample ? 1 : 0];
	if (op.id)
		return true;
	static const char *const vs =
			"#version 330 core\n"
			"void main()\n{\n"
			"\tvec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);\n"
			"\tgl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n}\n";
	// The averaged colour is quantised to the 5/6/5-bit value the lookup table is indexed by. With coverage, only
	// the samples with non-zero alpha count; a multisample target is read one sample at a time.
	std::string fs = "#version 330 core\n";
	if (per_sample)
	{
		fs += "uniform sampler2DMS u_srcms;\nuniform int u_samples;\nuniform ivec2 u_origin;\n"
				"#define SAMPLES u_samples\n#define ORIGIN u_origin\n"
				"#define FETCH(p, s) texelFetch(u_srcms, (p), (s))\n";
	}
	else
	{
		fs += "uniform sampler2D u_src;\n"
				"#define SAMPLES 1\n#define ORIGIN ivec2(0)\n"
				"#define FETCH(p, s) texelFetch(u_src, (p), 0)\n";
	}
	fs += "uniform int u_ds;\nuniform int u_flags;\nuniform int u_lut[128];\n"
			"out vec4 o_color;\n"
			"void main()\n{\n"
			"\tivec2 base = ivec2(gl_FragCoord.xy) * u_ds + ORIGIN;\n"
			"\tivec4 sum = ivec4(0);\n"
			"\tint count = 0;\n"
			"\tfor (int y = 0; y < u_ds; y++)\n\t{\n"
			"\t\tfor (int x = 0; x < u_ds; x++)\n\t\t{\n"
			"\t\t\tfor (int s = 0; s < SAMPLES; s++)\n\t\t\t{\n"
			"\t\t\t\tivec4 t = ivec4(FETCH(base + ivec2(x, y), s) * 255.0 + 0.5);\n"
			"\t\t\t\tif ((u_flags & 4) == 0 || t.a != 0)\n\t\t\t\t{\n"
			"\t\t\t\t\tsum += t;\n\t\t\t\t\tcount++;\n"
			"\t\t\t\t}\n\t\t\t}\n\t\t}\n\t}\n"
			"\tivec4 v = (count > 0) ? (sum / count) : ivec4(0);\n"
			"\tif ((u_flags & 1) != 0)\n\t{\n"
			"\t\tv.r = u_lut[v.r >> 3];\n\t\tv.g = u_lut[32 + (v.g >> 2)];\n\t\tv.b = u_lut[96 + (v.b >> 3)];\n"
			"\t}\n"
			"\tif ((u_flags & 2) != 0)\n\t\tv.a = 255;\n"
			"\tif ((u_flags & 4) != 0)\n\t{\n"
			"\t\tint total = u_ds * u_ds * SAMPLES;\n"
			"\t\tv.a = (count == 0) ? 0 : ((u_flags & 8) != 0) ? max(1, (255 * count + total / 2) / total) : 255;\n"
			"\t}\n"
			"\tvec4 c = vec4(v) / 255.0;\n"
			"\to_color = c;\n}\n";

	GLuint shaders[2] = { 0, 0 };
	const char *sources[2] = { vs, fs.c_str() };
	const GLenum types[2] = { GL_VERTEX_SHADER, GL_FRAGMENT_SHADER };
	bool ok = true;
	for (int i = 0; i < 2 && ok; i++)
	{
		shaders[i] = g_gl.CreateShader(types[i]);
		g_gl.ShaderSource(shaders[i], 1, &sources[i], nullptr);
		g_gl.CompileShader(shaders[i]);
		GLint status = 0;
		g_gl.GetShaderiv(shaders[i], GL_COMPILE_STATUS, &status);
		if (!status)
		{
			char log[2048];
			g_gl.GetShaderInfoLog(shaders[i], sizeof(log), nullptr, log);
			fprintf(stderr, "[retro_gpu_engine] output pass %s shader failed to compile:\n%s\n", i ? "fragment" : "vertex", log);
			ok = false;
		}
	}
	GLuint program = 0;
	if (ok)
	{
		program = g_gl.CreateProgram();
		g_gl.AttachShader(program, shaders[0]);
		g_gl.AttachShader(program, shaders[1]);
		g_gl.LinkProgram(program);
		GLint status = 0;
		g_gl.GetProgramiv(program, GL_LINK_STATUS, &status);
		if (!status)
		{
			char log[2048];
			g_gl.GetProgramInfoLog(program, sizeof(log), nullptr, log);
			fprintf(stderr, "[retro_gpu_engine] output pass link failed:\n%s\n", log);
			g_gl.DeleteProgram(program);
			program = 0;
			ok = false;
		}
	}
	for (GLuint sh : shaders)
		if (sh)
			g_gl.DeleteShader(sh);
	if (!ok)
	{
		m_failed = true;
		return false;
	}
	op.id = program;
	g_gl.UseProgram(program);
	g_gl.Uniform1i(g_gl.GetUniformLocation(program, per_sample ? "u_srcms" : "u_src"), 0);
	op.u_ds = g_gl.GetUniformLocation(program, "u_ds");
	op.u_flags = g_gl.GetUniformLocation(program, "u_flags");
	op.u_lut = g_gl.GetUniformLocation(program, "u_lut");
	op.u_samples = g_gl.GetUniformLocation(program, "u_samples");
	op.u_origin = g_gl.GetUniformLocation(program, "u_origin");
	if (!m_op_vao)
		g_gl.GenVertexArrays(1, &m_op_vao);
	return true;
}

// Read-back through a final pass: the rectangle (resolved if multisampled) is copied into a texture, a
// full-screen pass averages and maps it into a second texture of the output size, and that is read.
void retro_gpu_engine::output_pass(const osd::gpu_readback_rect &r, void *dest, int row_length)
{
	// a coverage read-back of a multisample target looks at its samples; any other is resolved first
	const bool per_sample = m_color_tex && r.coverage;
	if (!ensure_output_program(per_sample))
		return;
	const output_program &op = m_op[per_sample ? 1 : 0];
	const int ds = std::max(r.downsample, 1);
	const int ow = r.w / ds, oh = r.h / ds;

	auto make_target = [] (uint32_t &tex, uint32_t &fbo, int &cur_w, int &cur_h, int w, int h)
	{
		if (tex && cur_w == w && cur_h == h)
			return;
		if (fbo)
			g_gl.DeleteFramebuffers(1, &fbo);
		if (tex)
			g_gl.DeleteTextures(1, &tex);
		g_gl.GenTextures(1, &tex);
		g_gl.BindTexture(GL_TEXTURE_2D, tex);
		g_gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
		g_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		g_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		g_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		g_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		g_gl.GenFramebuffers(1, &fbo);
		g_gl.BindFramebuffer(GL_FRAMEBUFFER, fbo);
		g_gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
		cur_w = w;
		cur_h = h;
	};
	make_target(m_op_dst_tex, m_op_dst_fbo, m_op_dst_w, m_op_dst_h, ow, oh);

	g_gl.Disable(GL_SCISSOR_TEST);
	if (!per_sample)
	{
		// target (resolved) -> source texture
		make_target(m_op_src_tex, m_op_src_fbo, m_op_src_w, m_op_src_h, r.w, r.h);
		g_gl.BindFramebuffer(GL_READ_FRAMEBUFFER, m_fbo);
		g_gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, m_op_src_fbo);
		g_gl.BlitFramebuffer(r.x, r.y, r.x + r.w, r.y + r.h, 0, 0, r.w, r.h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
	}

	// source texture, or the samples of the target -> output texture
	g_gl.BindFramebuffer(GL_FRAMEBUFFER, m_op_dst_fbo);
	g_gl.Viewport(0, 0, ow, oh);
	g_gl.Disable(GL_BLEND);
	g_gl.Disable(GL_DEPTH_TEST);
	g_gl.ColorMask(1, 1, 1, 1);
	g_gl.UseProgram(op.id);
	g_gl.Uniform1i(op.u_ds, ds);
	g_gl.Uniform1i(op.u_flags, (r.channel_lut ? 1 : 0) | (r.opaque ? 2 : 0) | (r.coverage ? 4 : 0) | (r.coverage_share ? 8 : 0));
	if (per_sample)
	{
		g_gl.Uniform1i(op.u_samples, m_target.msaa_samples);
		g_gl.Uniform2i(op.u_origin, r.x, r.y);
	}
	if (r.channel_lut)
	{
		GLint lut[128];
		for (int i = 0; i < 128; i++)
			lut[i] = r.channel_lut[i];
		g_gl.Uniform1iv(op.u_lut, 128, lut);
	}
	g_gl.ActiveTexture(GL_TEXTURE0);
	if (per_sample)
		g_gl.BindTexture(GL_TEXTURE_2D_MULTISAMPLE, m_color_tex);
	else
		g_gl.BindTexture(GL_TEXTURE_2D, m_op_src_tex);
	g_gl.BindVertexArray(m_op_vao);
	g_gl.DrawArrays(GL_TRIANGLES, 0, 3);

	g_gl.PixelStorei(GL_PACK_ALIGNMENT, 4);
	g_gl.PixelStorei(GL_PACK_ROW_LENGTH, row_length);
	g_gl.ReadPixels(0, 0, ow, oh, GL_BGRA, GL_UNSIGNED_BYTE, dest);
	g_gl.PixelStorei(GL_PACK_ROW_LENGTH, 0);

	// back to the state the frame's commands expect
	g_gl.BindVertexArray(m_vao);
	g_gl.BindFramebuffer(GL_FRAMEBUFFER, m_fbo);
	g_gl.Viewport(0, 0, m_target.width, m_target.height);
	if (m_target.depth != osd::gpu_depth_mode::NONE)
		g_gl.Enable(GL_DEPTH_TEST);
}

// Reads one target rectangle to dest, which is a memory address or, with a pixel-pack buffer bound, an offset into
// it; row_length is the distance between destination rows in pixels. A multisampled target is resolved for that
// rectangle first.
void retro_gpu_engine::do_readback(const osd::gpu_readback_rect &r, void *dest, int row_length)
{
	if (!r.plain())
	{
		output_pass(r, dest, row_length);
		return;
	}
	if (m_resolve_fbo)
	{
		g_gl.BindFramebuffer(GL_READ_FRAMEBUFFER, m_fbo);
		g_gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, m_resolve_fbo);
		g_gl.BlitFramebuffer(r.x, r.y, r.x + r.w, r.y + r.h, r.x, r.y, r.x + r.w, r.y + r.h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
		g_gl.BindFramebuffer(GL_FRAMEBUFFER, m_resolve_fbo);
	}
	else
		g_gl.BindFramebuffer(GL_FRAMEBUFFER, m_fbo);
	g_gl.PixelStorei(GL_PACK_ROW_LENGTH, row_length);
	if (r.format == osd::gpu_readback_format::RG8)
	{
		g_gl.PixelStorei(GL_PACK_ALIGNMENT, 1);
		g_gl.ReadPixels(r.x, r.y, r.w, r.h, GL_RG, GL_UNSIGNED_BYTE, dest);
		g_gl.PixelStorei(GL_PACK_ALIGNMENT, 4);
	}
	else
	{
		g_gl.PixelStorei(GL_PACK_ALIGNMENT, 4);
		g_gl.ReadPixels(r.x, r.y, r.w, r.h, GL_BGRA, GL_UNSIGNED_BYTE, dest);
	}
	g_gl.PixelStorei(GL_PACK_ROW_LENGTH, 0);
	g_gl.BindFramebuffer(GL_FRAMEBUFFER, m_fbo);
}

// Forgets read-backs that were never collected.
void retro_gpu_engine::drop_pending()
{
	if (m_fence)
	{
		g_gl.DeleteSync(m_fence);
		m_fence = nullptr;
	}
	m_pending.clear();
}

// Waits for the deferred read-backs of the last frame and writes their pixels.
bool retro_gpu_engine::collect_frame()
{
	if (m_pending.empty())
		return true;
	if (!m_valid || m_failed)
	{
		m_pending.clear();
		return false;
	}
	const auto start = clock_type::now();
	scoped_context ctx(m_display, m_surface, m_context);
	if (!ctx.active)
	{
		m_pending.clear();
		return false;
	}

	bool ok = true;
	if (m_fence)
	{
		// one second is far beyond any frame; a longer wait means the GPU is lost
		const GLenum wait = g_gl.ClientWaitSync(m_fence, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ULL);
		if (wait == GL_TIMEOUT_EXPIRED || wait == GL_WAIT_FAILED)
		{
			log_problem("collect", "waiting for the GPU timed out");
			ok = false;
		}
		g_gl.DeleteSync(m_fence);
		m_fence = nullptr;
	}
	if (ok)
	{
		g_gl.BindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo);
		const uint8_t *src = static_cast<const uint8_t *>(g_gl.MapBufferRange(GL_PIXEL_PACK_BUFFER, 0, GLsizeiptr(m_pending_bytes), GL_MAP_READ_BIT));
		if (src)
		{
			for (const pending_readback &p : m_pending)
			{
				if (p.out16)
				{
					for (int y = 0; y < p.h; y++)
						memcpy(p.out16 + size_t(y) * p.stride, src + p.offset + size_t(y) * p.w * 2, size_t(p.w) * 2);
				}
				else
				{
					for (int y = 0; y < p.h; y++)
						memcpy(p.out + size_t(y) * p.stride, src + p.offset + size_t(y) * p.w * 4, size_t(p.w) * 4);
				}
			}
			g_gl.UnmapBuffer(GL_PIXEL_PACK_BUFFER);
		}
		else
		{
			log_problem("collect", "could not map the read-back buffer");
			ok = false;
		}
		g_gl.BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
	}
	m_pending.clear();
	m_stats.collect_ms = elapsed_ms(start);
	return ok;
}

bool retro_gpu_engine::submit_frame(const osd::gpu_frame &frame)
{
	if (!m_valid || m_failed)
		return false;

	m_stats = osd::gpu_engine_stats();

	if (!validate_frame(frame))
		return false;

	scoped_context ctx(m_display, m_surface, m_context);
	if (!ctx.active)
		return false;

	while (g_gl.GetError() != GL_NO_ERROR) { }

	// read-backs of an earlier frame that were never collected
	drop_pending();

	// result of an earlier frame's query that was still running when it returned
	if (m_timers && m_query_pending)
	{
		poll_gpu_timer(false);
		if (!m_query_pending)
			m_stats.gpu_ms = m_last_gpu_ms;
	}

	const auto upload_start = clock_type::now();
	osd::gpu_target_desc target = frame.target;
	fit_msaa(target);

	flush_deleted();
	ensure_target(target);
	if (m_failed)
		return false;
	ensure_state_texture(frame);

	g_gl.BindVertexArray(m_vao);
	g_gl.BindBuffer(GL_ARRAY_BUFFER, m_vbo);
	const int varyings = frame.varyings_per_vertex;
	const GLsizei stride = (5 + varyings) * 4;
	g_gl.BufferData(GL_ARRAY_BUFFER, GLsizeiptr(stride) * frame.vertex_count, frame.vertices, GL_STREAM_DRAW);
	g_gl.EnableVertexAttribArray(0);
	g_gl.VertexAttribPointer(0, 4, GL_FLOAT, 0, stride, (void *)0);
	g_gl.EnableVertexAttribArray(1);
	g_gl.VertexAttribIPointer(1, 1, GL_UNSIGNED_INT, stride, (void *)16);
	for (int g = 0; g < (varyings + 3) / 4; g++)
	{
		g_gl.EnableVertexAttribArray(2 + g);
		g_gl.VertexAttribPointer(2 + g, std::min(4, varyings - g * 4), GL_FLOAT, 0, stride, (void *)(uintptr_t)(20 + 16 * g));
	}

	for (int i = 0; i < frame.update_count; i++)
		apply_update(frame.updates[i]);
	m_stats.upload_ms = elapsed_ms(upload_start);

	const auto draw_start = clock_type::now();
	g_gl.BindFramebuffer(GL_FRAMEBUFFER, m_fbo);
	g_gl.Viewport(0, 0, target.width, target.height);
	g_gl.Disable(GL_SCISSOR_TEST);
	g_gl.Disable(GL_BLEND);
	g_gl.ColorMask(1, 1, 1, 1);
	const bool use_depth = target.depth != osd::gpu_depth_mode::NONE;
	if (use_depth)
		g_gl.Enable(GL_DEPTH_TEST);
	else
		g_gl.Disable(GL_DEPTH_TEST);
	const float order_scale = 1.0f / float(frame.prim_count + 1);
	const int rec_texels = std::max(1, (frame.state_stride + 3) / 4);

	bool scissor_on = false;
	const bool timing = m_timers && !m_query_pending;
	if (timing)
		g_gl.BeginQuery(GL_TIME_ELAPSED, m_query);

	for (int i = 0; i < frame.command_count; i++)
	{
		const osd::gpu_command &c = frame.commands[i];
		switch (c.type)
		{
		case osd::gpu_command_type::DRAW:
		{
			if (c.vertex_count == 0)
				break;
			const program *p = get_program(c.module, c.variant);
			if (!p)
			{
				if (timing)
					g_gl.EndQuery(GL_TIME_ELAPSED);
				return false;
			}
			const module &m = m_modules.find(c.module)->second;

			g_gl.UseProgram(p->gl_id);
			g_gl.Uniform2f(p->u_target_size, float(target.width), float(target.height));
			g_gl.Uniform1f(p->u_order_scale, order_scale);
			g_gl.Uniform1i(p->u_depth_order, target.depth == osd::gpu_depth_mode::ORDER ? 1 : 0);
			g_gl.Uniform1i(p->u_rec_texels, rec_texels);

			g_gl.ActiveTexture(GL_TEXTURE0);
			g_gl.BindTexture(GL_TEXTURE_2D, m_state_tex);
			for (int b = 0; b < m.binding_count; b++)
			{
				texture &t = m_textures.find(c.bindings[b])->second;
				ensure_texture(t);
				g_gl.ActiveTexture(GL_TEXTURE0 + 1 + b);
				g_gl.BindTexture(GL_TEXTURE_2D, t.gl_id);
			}

			apply_blend(c.blend);
			g_gl.ColorMask(c.color_write, c.color_write, c.color_write, c.color_write);
			if (target.depth == osd::gpu_depth_mode::ORDER)
			{
				g_gl.DepthFunc(GL_LESS);
				g_gl.DepthMask(1);
			}
			else if (use_depth)
			{
				g_gl.DepthFunc(depth_func_enum(c.depth_func));
				g_gl.DepthMask(c.depth_write ? 1 : 0);
			}

			if (target.msaa_samples > 0)
			{
				if (c.multisample)
					g_gl.Enable(GL_MULTISAMPLE);
				else
					g_gl.Disable(GL_MULTISAMPLE);
			}
			g_gl.DrawArrays(GL_TRIANGLES, c.first_vertex, c.vertex_count);
			m_stats.draw_calls++;
			break;
		}

		case osd::gpu_command_type::SET_CLIP:
			if (c.w <= 0 || c.h <= 0)
			{
				g_gl.Scissor(0, 0, 0, 0);
			}
			else
			{
				const int x1 = std::clamp(c.x, 0, target.width);
				const int y1 = std::clamp(c.y, 0, target.height);
				const int x2 = std::clamp(c.x + c.w, 0, target.width);
				const int y2 = std::clamp(c.y + c.h, 0, target.height);
				g_gl.Scissor(x1, y1, x2 - x1, y2 - y1);
			}
			g_gl.Enable(GL_SCISSOR_TEST);
			scissor_on = true;
			break;

		case osd::gpu_command_type::COPY_RECT:
			g_gl.Disable(GL_SCISSOR_TEST);
			g_gl.BindFramebuffer(GL_READ_FRAMEBUFFER, m_fbo);
			g_gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, m_fbo);
			g_gl.BlitFramebuffer(c.x, c.y, c.x + c.w, c.y + c.h, c.dx, c.dy, c.dx + c.w, c.dy + c.h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
			g_gl.BindFramebuffer(GL_FRAMEBUFFER, m_fbo);
			break;

		case osd::gpu_command_type::UPDATE_TEXTURE:
			apply_update(c.update);
			break;

		case osd::gpu_command_type::READBACK:
		{
			const auto rb_start = clock_type::now();
			g_gl.Disable(GL_SCISSOR_TEST);
			do_readback(c.readback, c.readback.format == osd::gpu_readback_format::RG8 ? static_cast<void *>(c.readback.out16) : static_cast<void *>(c.readback.out), c.readback.stride);
			if (scissor_on)
				g_gl.Enable(GL_SCISSOR_TEST);
			m_stats.readback_ms += elapsed_ms(rb_start);
			break;
		}

		case osd::gpu_command_type::CLEAR:
		{
			g_gl.Disable(GL_SCISSOR_TEST);
			g_gl.ColorMask(1, 1, 1, 1);
			g_gl.DepthMask(1);
			GLbitfield bits = 0;
			if (c.clear_color_enable)
			{
				g_gl.ClearColor(c.clear_color[0], c.clear_color[1], c.clear_color[2], c.clear_color[3]);
				bits |= GL_COLOR_BUFFER_BIT;
			}
			if (c.clear_depth_enable && use_depth)
			{
				g_gl.ClearDepth(1.0);
				bits |= GL_DEPTH_BUFFER_BIT;
			}
			if (bits)
				g_gl.Clear(bits);
			break;
		}
		}
	}
	g_gl.Disable(GL_SCISSOR_TEST);
	if (timing)
	{
		g_gl.EndQuery(GL_TIME_ELAPSED);
		m_query_pending = true;
	}
	// A frame that reads nothing back is not waited for. The commands are flushed (the context
	// release would do it too) and the GPU works on while emulation continues.
	const bool waits = frame.readback_count > 0 && !frame.defer_readback;
	if (waits)
		g_gl.Finish();
	else
		g_gl.Flush();
	m_stats.draw_ms = elapsed_ms(draw_start) - m_stats.readback_ms;
	if (timing && waits)
	{
		poll_gpu_timer(true);
		m_stats.gpu_ms = m_last_gpu_ms;
	}

	if (frame.readback_count > 0)
	{
		const auto read_start = clock_type::now();
		if (frame.defer_readback)
		{
			// every rectangle goes tightly packed into one pixel-pack buffer; collect_frame() copies the rows out
			m_pending.clear();
			size_t total = 0;
			for (int i = 0; i < frame.readback_count; i++)
			{
				const osd::gpu_readback_rect &r = frame.readbacks[i];
				const bool rg8 = r.format == osd::gpu_readback_format::RG8;
				pending_readback p;
				p.out = r.out;
				p.out16 = rg8 ? r.out16 : nullptr;
				p.stride = r.stride;
				p.w = r.w / std::max(r.downsample, 1);
				p.h = r.h / std::max(r.downsample, 1);
				p.offset = total;
				total += size_t(p.w) * p.h * (rg8 ? 2 : 4);
				m_pending.push_back(p);
			}
			if (!m_pbo)
				g_gl.GenBuffers(1, &m_pbo);
			g_gl.BindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo);
			g_gl.BufferData(GL_PIXEL_PACK_BUFFER, GLsizeiptr(total), nullptr, GL_STREAM_READ);
			m_pbo_size = total;
			m_pending_bytes = total;
			for (int i = 0; i < frame.readback_count; i++)
				do_readback(frame.readbacks[i], reinterpret_cast<void *>(uintptr_t(m_pending[i].offset)), m_pending[i].w);
			g_gl.BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
			m_fence = g_gl.FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
			g_gl.Flush();
		}
		else
		{
			for (int i = 0; i < frame.readback_count; i++)
			{
				const osd::gpu_readback_rect &r = frame.readbacks[i];
				do_readback(r, r.format == osd::gpu_readback_format::RG8 ? static_cast<void *>(r.out16) : static_cast<void *>(r.out), r.stride);
			}
		}
		m_stats.readback_ms += elapsed_ms(read_start);
	}

	const GLenum error = g_gl.GetError();
	if (error != GL_NO_ERROR)
	{
		char detail[64];
		snprintf(detail, sizeof(detail), "GL error 0x%x during the frame", error);
		log_problem("frame", detail);
		return false;
	}
	return true;
}
