// license:BSD-3-Clause
// copyright-holders:TrickoUK
/***************************************************************************

    retro_gpu_target.cpp

    See retro_gpu_target.h for the design rationale.

    EGL and GL are loaded entirely via dlopen()/dlsym() at runtime rather
    than linked at build time: the build host for this fork doesn't have
    the EGL/GL -devel packages installed (an atomic/immutable desktop,
    where layering them requires a reboot - undesirable just for a build
    dependency), and dlopen avoids that entirely. It also means this
    feature degrades gracefully to "unavailable" (is_valid() == false) on
    any system without a working OpenGL/EGL install, exactly matching the
    required software-fallback behavior, rather than being a hard link-time
    dependency of the whole core.

    Because of this, no <EGL/egl.h>/<GL/gl.h> system headers are used
    either - only the small subset of types/constants/function signatures
    actually needed are declared locally below.

***************************************************************************/
#include "retro_gpu_target.h"

#include <dlfcn.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>

//**************************************************************************
//  Minimal local EGL/GL declarations (see file header comment for why)
//**************************************************************************

typedef int32_t EGLint;
typedef unsigned int EGLBoolean;
typedef unsigned int EGLenum;
typedef void *EGLConfig;
typedef void *(*PFNEGLGETPROCADDRESSPROC)(const char *);
typedef void *(*PFNEGLGETDISPLAYPROC)(void *native_display);
typedef void *(*PFNEGLGETPLATFORMDISPLAYPROC)(EGLenum platform, void *native_display, const intptr_t *attrib_list);
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
typedef EGLBoolean (*PFNEGLINITIALIZEPROC)(EGLDisplay, EGLint *, EGLint *);
typedef EGLBoolean (*PFNEGLBINDAPIPROC)(EGLenum);
typedef EGLBoolean (*PFNEGLCHOOSECONFIGPROC)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
typedef EGLSurface (*PFNEGLCREATEPBUFFERSURFACEPROC)(EGLDisplay, EGLConfig, const EGLint *);
typedef EGLContext (*PFNEGLCREATECONTEXTPROC)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
typedef EGLBoolean (*PFNEGLMAKECURRENTPROC)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
typedef EGLBoolean (*PFNEGLDESTROYCONTEXTPROC)(EGLDisplay, EGLContext);
typedef EGLBoolean (*PFNEGLDESTROYSURFACEPROC)(EGLDisplay, EGLSurface);
typedef EGLBoolean (*PFNEGLTERMINATEPROC)(EGLDisplay);
typedef EGLint (*PFNEGLGETERRORPROC)(void);
typedef EGLContext (*PFNEGLGETCURRENTCONTEXTPROC)(void);
typedef EGLDisplay (*PFNEGLGETCURRENTDISPLAYPROC)(void);
typedef EGLSurface (*PFNEGLGETCURRENTSURFACEPROC)(EGLint);
#define EGL_DRAW_ 0x3059
#define EGL_READ_ 0x305A

#define EGL_NO_DISPLAY_ ((EGLDisplay)0)
#define EGL_NO_CONTEXT_ ((EGLContext)0)
#define EGL_NO_SURFACE_ ((EGLSurface)0)
#define EGL_DEFAULT_DISPLAY_ ((void*)0)
#define EGL_OPENGL_API_ 0x30A2
#define EGL_SURFACE_TYPE_ 0x3033
#define EGL_PBUFFER_BIT_ 0x0001
#define EGL_RENDERABLE_TYPE_ 0x3040
#define EGL_OPENGL_BIT_ 0x0008
#define EGL_RED_SIZE_ 0x3024
#define EGL_GREEN_SIZE_ 0x3023
#define EGL_BLUE_SIZE_ 0x3022
#define EGL_ALPHA_SIZE_ 0x3021
#define EGL_NONE_ 0x3038
#define EGL_WIDTH_ 0x3057
#define EGL_HEIGHT_ 0x3056
#define EGL_CONTEXT_MAJOR_VERSION_ 0x3098
#define EGL_CONTEXT_MINOR_VERSION_ 0x30FB
#define EGL_CONTEXT_OPENGL_PROFILE_MASK_ 0x30FD
#define EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_ 0x00000001

typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef unsigned char GLboolean;
typedef float GLfloat;
typedef ptrdiff_t GLsizeiptr;
typedef char GLchar;
typedef unsigned int GLbitfield;

#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_TRIANGLES 0x0004
#define GL_TEXTURE_2D 0x0DE1
#define GL_UNSIGNED_BYTE 0x1401
#define GL_FLOAT 0x1406
#define GL_RGBA 0x1908
#define GL_BGRA 0x80E1
#define GL_RGBA8 0x8058
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_NEAREST 0x2600
#define GL_TEXTURE0 0x84C0
#define GL_FRAMEBUFFER 0x8D40
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_ARRAY_BUFFER 0x8892
#define GL_DYNAMIC_DRAW 0x88E8
#define GL_BLEND 0x0BE2
#define GL_FUNC_ADD 0x8006
#define GL_FUNC_REVERSE_SUBTRACT 0x800B
#define GL_ONE 1
#define GL_CONSTANT_ALPHA 0x8003
#define GL_ZERO 0
#define GL_SRC_ALPHA 0x0302
#define GL_SCISSOR_TEST 0x0C11

typedef void (*PFNGLGENFRAMEBUFFERSPROC)(GLsizei, GLuint *);
typedef void (*PFNGLBINDFRAMEBUFFERPROC)(GLenum, GLuint);
typedef void (*PFNGLFRAMEBUFFERTEXTURE2DPROC)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef GLenum (*PFNGLCHECKFRAMEBUFFERSTATUSPROC)(GLenum);
typedef GLuint (*PFNGLCREATESHADERPROC)(GLenum);
typedef void (*PFNGLSHADERSOURCEPROC)(GLuint, GLsizei, const GLchar **, const GLint *);
typedef void (*PFNGLCOMPILESHADERPROC)(GLuint);
typedef void (*PFNGLGETSHADERIVPROC)(GLuint, GLenum, GLint *);
typedef void (*PFNGLGETSHADERINFOLOGPROC)(GLuint, GLsizei, GLsizei *, GLchar *);
typedef GLuint (*PFNGLCREATEPROGRAMPROC)(void);
typedef void (*PFNGLATTACHSHADERPROC)(GLuint, GLuint);
typedef void (*PFNGLBINDATTRIBLOCATIONPROC)(GLuint, GLuint, const GLchar *);
typedef void (*PFNGLLINKPROGRAMPROC)(GLuint);
typedef void (*PFNGLGETPROGRAMIVPROC)(GLuint, GLenum, GLint *);
typedef void (*PFNGLGETPROGRAMINFOLOGPROC)(GLuint, GLsizei, GLsizei *, GLchar *);
typedef void (*PFNGLUSEPROGRAMPROC)(GLuint);
typedef GLint (*PFNGLGETUNIFORMLOCATIONPROC)(GLuint, const GLchar *);
typedef void (*PFNGLUNIFORM2FPROC)(GLint, GLfloat, GLfloat);
typedef void (*PFNGLUNIFORM1IPROC)(GLint, GLint);
typedef void (*PFNGLGENVERTEXARRAYSPROC)(GLsizei, GLuint *);
typedef void (*PFNGLBINDVERTEXARRAYPROC)(GLuint);
typedef void (*PFNGLGENBUFFERSPROC)(GLsizei, GLuint *);
typedef void (*PFNGLBINDBUFFERPROC)(GLenum, GLuint);
typedef void (*PFNGLBUFFERDATAPROC)(GLenum, GLsizeiptr, const void *, GLenum);
typedef void (*PFNGLENABLEVERTEXATTRIBARRAYPROC)(GLuint);
typedef void (*PFNGLVERTEXATTRIBPOINTERPROC)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *);
typedef void (*PFNGLACTIVETEXTUREPROC)(GLenum);
typedef void (*PFNGLBLENDEQUATIONPROC)(GLenum);
typedef void (*PFNGLBLENDFUNCPROC)(GLenum, GLenum);
typedef void (*PFNGLBLENDCOLORPROC)(GLfloat, GLfloat, GLfloat, GLfloat);
typedef void (*PFNGLGENTEXTURESPROC)(GLsizei, GLuint *);
typedef void (*PFNGLBINDTEXTUREPROC)(GLenum, GLuint);
typedef void (*PFNGLTEXIMAGE2DPROC)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
typedef void (*PFNGLTEXPARAMETERIPROC)(GLenum, GLenum, GLint);
typedef void (*PFNGLVIEWPORTPROC)(GLint, GLint, GLsizei, GLsizei);
typedef void (*PFNGLCLEARCOLORPROC)(GLfloat, GLfloat, GLfloat, GLfloat);
typedef void (*PFNGLCLEARPROC)(GLbitfield);
typedef void (*PFNGLDRAWARRAYSPROC)(GLenum, GLint, GLsizei);
typedef void (*PFNGLREADPIXELSPROC)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
typedef void (*PFNGLFINISHPROC)(void);
typedef void (*PFNGLENABLEPROC)(GLenum);
typedef void (*PFNGLDISABLEPROC)(GLenum);
typedef void (*PFNGLSCISSORPROC)(GLint, GLint, GLsizei, GLsizei);
typedef const GLchar *(*PFNGLGETSTRINGPROC)(GLenum);
typedef GLenum (*PFNGLGETERRORPROC)(void);
#define GL_VENDOR 0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02

namespace {

struct gl_dispatch
{
	PFNGLGENFRAMEBUFFERSPROC GenFramebuffers = nullptr;
	PFNGLBINDFRAMEBUFFERPROC BindFramebuffer = nullptr;
	PFNGLFRAMEBUFFERTEXTURE2DPROC FramebufferTexture2D = nullptr;
	PFNGLCHECKFRAMEBUFFERSTATUSPROC CheckFramebufferStatus = nullptr;
	PFNGLCREATESHADERPROC CreateShader = nullptr;
	PFNGLSHADERSOURCEPROC ShaderSource = nullptr;
	PFNGLCOMPILESHADERPROC CompileShader = nullptr;
	PFNGLGETSHADERIVPROC GetShaderiv = nullptr;
	PFNGLGETSHADERINFOLOGPROC GetShaderInfoLog = nullptr;
	PFNGLCREATEPROGRAMPROC CreateProgram = nullptr;
	PFNGLATTACHSHADERPROC AttachShader = nullptr;
	PFNGLBINDATTRIBLOCATIONPROC BindAttribLocation = nullptr;
	PFNGLLINKPROGRAMPROC LinkProgram = nullptr;
	PFNGLGETPROGRAMIVPROC GetProgramiv = nullptr;
	PFNGLGETPROGRAMINFOLOGPROC GetProgramInfoLog = nullptr;
	PFNGLUSEPROGRAMPROC UseProgram = nullptr;
	PFNGLGETUNIFORMLOCATIONPROC GetUniformLocation = nullptr;
	PFNGLUNIFORM2FPROC Uniform2f = nullptr;
	PFNGLUNIFORM1IPROC Uniform1i = nullptr;
	PFNGLGENVERTEXARRAYSPROC GenVertexArrays = nullptr;
	PFNGLBINDVERTEXARRAYPROC BindVertexArray = nullptr;
	PFNGLGENBUFFERSPROC GenBuffers = nullptr;
	PFNGLBINDBUFFERPROC BindBuffer = nullptr;
	PFNGLBUFFERDATAPROC BufferData = nullptr;
	PFNGLENABLEVERTEXATTRIBARRAYPROC EnableVertexAttribArray = nullptr;
	PFNGLVERTEXATTRIBPOINTERPROC VertexAttribPointer = nullptr;
	PFNGLACTIVETEXTUREPROC ActiveTexture = nullptr;
	PFNGLBLENDEQUATIONPROC BlendEquation = nullptr;
	PFNGLBLENDFUNCPROC BlendFunc = nullptr;
	PFNGLBLENDCOLORPROC BlendColor = nullptr;
	PFNGLGENTEXTURESPROC GenTextures = nullptr;
	PFNGLBINDTEXTUREPROC BindTexture = nullptr;
	PFNGLTEXIMAGE2DPROC TexImage2D = nullptr;
	PFNGLTEXPARAMETERIPROC TexParameteri = nullptr;
	PFNGLVIEWPORTPROC Viewport = nullptr;
	PFNGLCLEARCOLORPROC ClearColor = nullptr;
	PFNGLCLEARPROC Clear = nullptr;
	PFNGLDRAWARRAYSPROC DrawArrays = nullptr;
	PFNGLREADPIXELSPROC ReadPixels = nullptr;
	PFNGLFINISHPROC Finish = nullptr;
	PFNGLENABLEPROC Enable = nullptr;
	PFNGLDISABLEPROC Disable = nullptr;
	PFNGLSCISSORPROC Scissor = nullptr;
	PFNGLGETSTRINGPROC GetString = nullptr;
	PFNGLGETERRORPROC GetError = nullptr;
};

struct egl_dispatch
{
	void *lib_egl = nullptr;
	void *lib_gl = nullptr;
	PFNEGLGETPROCADDRESSPROC GetProcAddress = nullptr;
	PFNEGLGETDISPLAYPROC GetDisplay = nullptr;
	PFNEGLGETPLATFORMDISPLAYPROC GetPlatformDisplay = nullptr;
	PFNEGLINITIALIZEPROC Initialize = nullptr;
	PFNEGLBINDAPIPROC BindAPI = nullptr;
	PFNEGLCHOOSECONFIGPROC ChooseConfig = nullptr;
	PFNEGLCREATEPBUFFERSURFACEPROC CreatePbufferSurface = nullptr;
	PFNEGLCREATECONTEXTPROC CreateContext = nullptr;
	PFNEGLMAKECURRENTPROC MakeCurrent = nullptr;
	PFNEGLDESTROYCONTEXTPROC DestroyContext = nullptr;
	PFNEGLDESTROYSURFACEPROC DestroySurface = nullptr;
	PFNEGLTERMINATEPROC Terminate = nullptr;
	PFNEGLGETERRORPROC GetError = nullptr;
	PFNEGLGETCURRENTCONTEXTPROC GetCurrentContext = nullptr;
	PFNEGLGETCURRENTDISPLAYPROC GetCurrentDisplay = nullptr;
	PFNEGLGETCURRENTSURFACEPROC GetCurrentSurface = nullptr;
};

egl_dispatch g_egl;
gl_dispatch g_gl;
bool g_dispatch_loaded = false;
bool g_dispatch_load_failed = false;

bool load_egl_dispatch()
{
	g_egl.lib_egl = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);
	if (!g_egl.lib_egl)
		g_egl.lib_egl = dlopen("libEGL.so", RTLD_NOW | RTLD_GLOBAL);
	if (!g_egl.lib_egl)
		return false;

	g_egl.lib_gl = dlopen("libGL.so.1", RTLD_NOW | RTLD_GLOBAL);
	if (!g_egl.lib_gl)
		g_egl.lib_gl = dlopen("libGL.so", RTLD_NOW | RTLD_GLOBAL);
	if (!g_egl.lib_gl)
		return false;

#define LOAD_EGL(name) g_egl.name = (decltype(g_egl.name))dlsym(g_egl.lib_egl, "egl" #name); if (!g_egl.name) return false;
	LOAD_EGL(GetProcAddress);
	LOAD_EGL(GetDisplay);
	LOAD_EGL(GetPlatformDisplay);
	LOAD_EGL(Initialize);
	LOAD_EGL(BindAPI);
	LOAD_EGL(ChooseConfig);
	LOAD_EGL(CreatePbufferSurface);
	LOAD_EGL(CreateContext);
	LOAD_EGL(MakeCurrent);
	LOAD_EGL(DestroyContext);
	LOAD_EGL(DestroySurface);
	LOAD_EGL(Terminate);
	LOAD_EGL(GetError);
	LOAD_EGL(GetCurrentContext);
	LOAD_EGL(GetCurrentDisplay);
	LOAD_EGL(GetCurrentSurface);
#undef LOAD_EGL
	return true;
}

bool load_gl_dispatch()
{
	// GL 1.x entry points (GetString) are exported directly by libGL;
	// everything GL 2.0+ must go through eglGetProcAddress.
	g_gl.GetString = (PFNGLGETSTRINGPROC)dlsym(g_egl.lib_gl, "glGetString");
	if (!g_gl.GetString)
		return false;
	g_gl.GetError = (PFNGLGETERRORPROC)dlsym(g_egl.lib_gl, "glGetError");
	if (!g_gl.GetError)
		return false;

#define LOAD_GL(name) g_gl.name = (decltype(g_gl.name))g_egl.GetProcAddress("gl" #name); if (!g_gl.name) { fprintf(stderr, "[retro_gpu_target] missing GL proc: gl%s\n", #name); return false; }
	LOAD_GL(GenFramebuffers); LOAD_GL(BindFramebuffer); LOAD_GL(FramebufferTexture2D);
	LOAD_GL(CheckFramebufferStatus); LOAD_GL(CreateShader); LOAD_GL(ShaderSource);
	LOAD_GL(CompileShader); LOAD_GL(GetShaderiv); LOAD_GL(GetShaderInfoLog);
	LOAD_GL(CreateProgram); LOAD_GL(AttachShader); LOAD_GL(BindAttribLocation);
	LOAD_GL(LinkProgram); LOAD_GL(GetProgramiv); LOAD_GL(GetProgramInfoLog);
	LOAD_GL(UseProgram); LOAD_GL(GetUniformLocation); LOAD_GL(Uniform2f); LOAD_GL(Uniform1i);
	LOAD_GL(GenVertexArrays); LOAD_GL(BindVertexArray);
	LOAD_GL(GenBuffers); LOAD_GL(BindBuffer); LOAD_GL(BufferData);
	LOAD_GL(EnableVertexAttribArray); LOAD_GL(VertexAttribPointer);
	LOAD_GL(ActiveTexture); LOAD_GL(BlendEquation); LOAD_GL(BlendFunc); LOAD_GL(BlendColor);
	LOAD_GL(GenTextures); LOAD_GL(BindTexture); LOAD_GL(TexImage2D); LOAD_GL(TexParameteri);
	LOAD_GL(Viewport); LOAD_GL(ClearColor); LOAD_GL(Clear); LOAD_GL(DrawArrays);
	LOAD_GL(ReadPixels); LOAD_GL(Finish); LOAD_GL(Enable); LOAD_GL(Disable); LOAD_GL(Scissor);
#undef LOAD_GL
	return true;
}

bool ensure_dispatch_loaded()
{
	if (g_dispatch_loaded)
		return true;
	if (g_dispatch_load_failed)
		return false;
	if (!load_egl_dispatch())
	{
		fprintf(stderr, "[retro_gpu_target] failed to dlopen libEGL/libGL - GPU rendering unavailable\n");
		g_dispatch_load_failed = true;
		return false;
	}
	g_dispatch_loaded = true;
	return true;
}

const char *vertex_shader_src =
	"#version 330 core\n"
	"layout(location=0) in vec2 a_pos;\n"
	"layout(location=1) in vec4 a_color;\n"
	"layout(location=2) in vec2 a_uv;\n"
	"uniform vec2 u_target_size;\n"
	"out vec4 v_color;\n"
	"out vec2 v_uv;\n"
	"void main() {\n"
	"    v_color = a_color;\n"
	"    v_uv = a_uv;\n"
	"    float ndc_x = (a_pos.x / u_target_size.x) * 2.0 - 1.0;\n"
	"    float ndc_y = 1.0 - (a_pos.y / u_target_size.y) * 2.0;\n"
	"    gl_Position = vec4(ndc_x, ndc_y, 0.0, 1.0);\n"
	"}\n";

/* Textured mode treats v_color as a PS1-style modulation factor (0.5 = the
 * neutral/no-op shade PS1 games use for "raw" unshaded textures - see
 * psxgpu_device::gpu_submit_flat_textured_polygon()) rather than an alpha
 * multiplier, matching the CPU rasterizer's SHADEDPIXEL/TRANSPARENTPIXEL
 * macros (psx.cpp) which multiply the texel by the vertex shade centered
 * at 0x80/128. Texels with zero alpha (n_bgr == 0 when decoded, see
 * gpu_decode_texture_page()) are the PS1 GPU's "transparent, don't draw"
 * marker and are discarded rather than blended. */
const char *fragment_shader_src =
	"#version 330 core\n"
	"in vec4 v_color;\n"
	"in vec2 v_uv;\n"
	"uniform sampler2D u_tex;\n"
	"uniform int u_textured;\n"
	"out vec4 frag_color;\n"
	"void main() {\n"
	"    if (u_textured != 0) {\n"
	"        vec4 texel = texelFetch(u_tex, ivec2(v_uv), 0);\n"
	"        if (texel.a == 0.0) discard;\n"
	"        frag_color = vec4(clamp(texel.rgb * (v_color.rgb * 2.0), 0.0, 1.0), 1.0);\n"
	"    } else {\n"
	"        frag_color = v_color;\n"
	"    }\n"
	"}\n";

// Makes the target's EGL context current for the lifetime of this object,
// restoring whatever was current on the calling thread beforehand when it
// goes out of scope. Used individually inside every public entry point
// below (never held across multiple calls) - see the header comment on
// why this must stay scoped this narrowly: RetroArch's frontend can
// perform its own EGL work synchronously from inside a MAME core call
// (e.g. reacting to our scaled-resolution screen.configure()), and a
// wider hold was found to eventually corrupt shared driver state even
// with careful lazy reacquisition (see CLAUDE.md/plan file Phase 2
// status for the full investigation). The resulting per-call
// eglMakeCurrent overhead is a known, accepted performance cost for now.
struct scoped_context
{
	EGLDisplay saved_display;
	EGLSurface saved_draw_surface;
	EGLSurface saved_read_surface;
	EGLContext saved_context;
	bool active;

	scoped_context(EGLDisplay display, EGLSurface surface, EGLContext context)
	{
		saved_display = g_egl.GetCurrentDisplay();
		saved_draw_surface = g_egl.GetCurrentSurface(EGL_DRAW_);
		saved_read_surface = g_egl.GetCurrentSurface(EGL_READ_);
		saved_context = g_egl.GetCurrentContext();
		active = g_egl.MakeCurrent(display, surface, surface, context);
		if (!active)
			fprintf(stderr, "[retro_gpu_target] scoped_context: eglMakeCurrent failed: 0x%x\n", g_egl.GetError());
	}

	~scoped_context()
	{
		if (active)
			g_egl.MakeCurrent(saved_display, saved_draw_surface, saved_read_surface, saved_context);
	}

	scoped_context(const scoped_context &) = delete;
	scoped_context &operator=(const scoped_context &) = delete;
};

} // anonymous namespace

retro_gpu_target::retro_gpu_target()
	: m_display(nullptr), m_context(nullptr), m_surface(nullptr), m_valid(false)
	, m_fbo(0), m_color_tex(0), m_fbo_width(0), m_fbo_height(0)
	, m_vram_tex(0), m_vram_tex_width(0), m_vram_tex_height(0)
	, m_program(0), m_vao(0), m_vbo(0)
	, m_u_target_size_loc(-1), m_u_textured_loc(-1)
	, m_current_blend(osd::gpu_blend_mode::NONE)
	, m_scissor_enabled(false), m_scissor_x(0), m_scissor_y(0), m_scissor_w(0), m_scissor_h(0)
{
	m_valid = init_context();
}

retro_gpu_target::~retro_gpu_target()
{
	if (m_display && g_dispatch_loaded)
	{
		g_egl.MakeCurrent(m_display, EGL_NO_SURFACE_, EGL_NO_SURFACE_, EGL_NO_CONTEXT_);
		if (m_context)
			g_egl.DestroyContext(m_display, m_context);
		if (m_surface)
			g_egl.DestroySurface(m_display, m_surface);
		g_egl.Terminate(m_display);
	}
}

bool retro_gpu_target::init_context()
{
	setvbuf(stderr, nullptr, _IONBF, 0);

	if (!ensure_dispatch_loaded())
		return false;

	// Use the surfaceless platform explicitly, not eglGetDisplay's
	// "default" platform - the latter can end up resolving to the same
	// underlying windowing-system connection RetroArch's own EGL/GL setup
	// uses (this host runs under Wayland), and creating/tearing down a
	// context on it BEFORE RetroArch has created its own was observed to
	// corrupt RetroArch's later eglMakeCurrent/eglSwapInterval (EGL_BAD_
	// CONTEXT, then a crash) even with careful current-context save/restore
	// around our own calls. EGL_PLATFORM_SURFACELESS_MESA has no windowing
	// system involvement at all, so there's nothing to interfere with
	// regardless of creation order.
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

	// Save whatever context is current on this thread before taking it over
	// to query GL strings/compile shaders below - RetroArch may already have
	// created and bound its own context by the time this lazily runs (see
	// gpu_active()), and previously this function only ever released to "no
	// context" afterward instead of restoring RetroArch's, leaving nothing
	// current on the thread and causing RetroArch's next eglSwapInterval to
	// fail with EGL_BAD_CONTEXT. This runs once, before m_display/m_surface/
	// m_context (which scoped_context needs) are fully set up, so it can't
	// just use that helper - inlined here instead.
	EGLDisplay prior_display = g_egl.GetCurrentDisplay();
	EGLSurface prior_draw = g_egl.GetCurrentSurface(EGL_DRAW_);
	EGLSurface prior_read = g_egl.GetCurrentSurface(EGL_READ_);
	EGLContext prior_context = g_egl.GetCurrentContext();

	if (!g_egl.MakeCurrent(m_display, m_surface, m_surface, m_context))
		return false;

	if (!load_gl_dispatch())
		return false;

	fprintf(stderr, "[retro_gpu_target] initialized: %s / %s / %s\n",
		(const char*)g_gl.GetString(GL_VENDOR), (const char*)g_gl.GetString(GL_RENDERER), (const char*)g_gl.GetString(GL_VERSION));

	m_program = compile_program();
	if (!m_program)
		return false;

	g_gl.UseProgram(m_program);
	m_u_target_size_loc = g_gl.GetUniformLocation(m_program, "u_target_size");
	m_u_textured_loc = g_gl.GetUniformLocation(m_program, "u_textured");
	GLint tex_loc = g_gl.GetUniformLocation(m_program, "u_tex");
	g_gl.Uniform1i(tex_loc, 0);

	g_gl.GenVertexArrays(1, &m_vao);
	g_gl.BindVertexArray(m_vao);
	g_gl.GenBuffers(1, &m_vbo);
	g_gl.BindBuffer(GL_ARRAY_BUFFER, m_vbo);
	g_gl.EnableVertexAttribArray(0);
	g_gl.VertexAttribPointer(0, 2, GL_FLOAT, 0, sizeof(osd::gpu_vertex), (void*)offsetof(osd::gpu_vertex, x));
	g_gl.EnableVertexAttribArray(1);
	g_gl.VertexAttribPointer(1, 4, GL_FLOAT, 0, sizeof(osd::gpu_vertex), (void*)offsetof(osd::gpu_vertex, r));
	g_gl.EnableVertexAttribArray(2);
	g_gl.VertexAttribPointer(2, 2, GL_FLOAT, 0, sizeof(osd::gpu_vertex), (void*)offsetof(osd::gpu_vertex, u));

	g_gl.GenTextures(1, &m_vram_tex);

	// Restore whatever was current before (RetroArch's own context, if it
	// had already created one by this point) rather than leaving the thread
	// with nothing current at all. If nothing was current before either
	// (this ran before RetroArch had set up its own context yet), fall back
	// to explicitly releasing to no-context, matching prior behavior for
	// that case.
	if (prior_display)
		g_egl.MakeCurrent(prior_display, prior_draw, prior_read, prior_context);
	else
		g_egl.MakeCurrent(m_display, EGL_NO_SURFACE_, EGL_NO_SURFACE_, EGL_NO_CONTEXT_);

	return true;
}

uint32_t retro_gpu_target::compile_program()
{
	GLuint vs = g_gl.CreateShader(GL_VERTEX_SHADER);
	g_gl.ShaderSource(vs, 1, &vertex_shader_src, nullptr);
	g_gl.CompileShader(vs);
	GLint ok = 0;
	g_gl.GetShaderiv(vs, GL_COMPILE_STATUS, &ok);
	if (!ok)
	{
		char log[2048];
		g_gl.GetShaderInfoLog(vs, sizeof(log), nullptr, log);
		fprintf(stderr, "[retro_gpu_target] vertex shader compile failed: %s\n", log);
		return 0;
	}

	GLuint fs = g_gl.CreateShader(GL_FRAGMENT_SHADER);
	g_gl.ShaderSource(fs, 1, &fragment_shader_src, nullptr);
	g_gl.CompileShader(fs);
	g_gl.GetShaderiv(fs, GL_COMPILE_STATUS, &ok);
	if (!ok)
	{
		char log[2048];
		g_gl.GetShaderInfoLog(fs, sizeof(log), nullptr, log);
		fprintf(stderr, "[retro_gpu_target] fragment shader compile failed: %s\n", log);
		return 0;
	}

	GLuint prog = g_gl.CreateProgram();
	g_gl.AttachShader(prog, vs);
	g_gl.AttachShader(prog, fs);
	g_gl.LinkProgram(prog);
	GLint linked = 0;
	g_gl.GetProgramiv(prog, GL_LINK_STATUS, &linked);
	if (!linked)
	{
		char log[2048];
		g_gl.GetProgramInfoLog(prog, sizeof(log), nullptr, log);
		fprintf(stderr, "[retro_gpu_target] program link failed: %s\n", log);
		return 0;
	}
	return prog;
}

void retro_gpu_target::resize_target(int width, int height)
{
	if (m_fbo && m_fbo_width == width && m_fbo_height == height)
		return;

	if (!m_fbo)
		g_gl.GenFramebuffers(1, &m_fbo);
	if (!m_color_tex)
		g_gl.GenTextures(1, &m_color_tex);

	g_gl.BindFramebuffer(GL_FRAMEBUFFER, m_fbo);
	g_gl.BindTexture(GL_TEXTURE_2D, m_color_tex);
	g_gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	g_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	g_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	g_gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_color_tex, 0);

	// Only cleared here, on (re)allocation - not every begin_frame(). This
	// target models PS1 VRAM, which is persistent: real hardware/the
	// original software rasterizer only erase pixels a game explicitly
	// overdraws (typically via its own fill-rectangle GP0 command). Clearing
	// on every frame regardless wrongly assumed every game fully redraws
	// 100% of the visible area every single refresh - false in practice
	// (observed: alternating full/near-black frames on Brave Blade, since
	// whatever it doesn't resubmit in a given frame was being wiped instead
	// of persisting like real VRAM).
	g_gl.ClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	g_gl.Clear(GL_COLOR_BUFFER_BIT);

	m_fbo_width = width;
	m_fbo_height = height;
}

void retro_gpu_target::begin_frame(int width, int height)
{
	if (!m_valid)
		return;

	scoped_context ctx(m_display, m_surface, m_context);
	if (!ctx.active)
		return;

	resize_target(width, height);
	g_gl.BindFramebuffer(GL_FRAMEBUFFER, m_fbo);
	g_gl.Viewport(0, 0, width, height);
	g_gl.UseProgram(m_program);
	g_gl.Uniform2f(m_u_target_size_loc, (float)width, (float)height);
	g_gl.BindVertexArray(m_vao);
	m_current_blend = osd::gpu_blend_mode::NONE;
	g_gl.Disable(GL_BLEND);
}

// No-ops: with per-call scoped_context above, our context is never left
// bound between calls, so there is nothing for these to yield/resume - see
// the interface's default (osd::gpu_render_target::yield_context()) for
// the general contract. Kept as explicit overrides (rather than removing
// them and falling back to the base class) so the call sites in
// psxgpu_device::updatevisiblearea() don't need to change if a future,
// safe way to hold the context wider is found.
void retro_gpu_target::yield_context()
{
}

void retro_gpu_target::resume_context()
{
}

void retro_gpu_target::set_clip_rect(int x1, int y1, int x2, int y2)
{
	if (!m_valid)
		return;

	scoped_context ctx(m_display, m_surface, m_context);
	if (!ctx.active)
		return;

	// x1,y1,x2,y2 are inclusive, in the same top-left-origin target-pixel
	// space as gpu_vertex positions. glScissor's (x,y) is the *lower*-left
	// corner in OpenGL's bottom-up window space - the same flip
	// end_frame_and_readback() undoes on readback - so the box's top edge
	// (y1) maps to the *larger* GL y value and vice versa.
	m_scissor_x = x1;
	m_scissor_y = m_fbo_height - (y2 + 1);
	m_scissor_w = (x2 - x1) + 1;
	m_scissor_h = (y2 - y1) + 1;
	if (m_scissor_w < 0) m_scissor_w = 0;
	if (m_scissor_h < 0) m_scissor_h = 0;
	m_scissor_enabled = true;

	g_gl.Enable(GL_SCISSOR_TEST);
	g_gl.Scissor(m_scissor_x, m_scissor_y, m_scissor_w, m_scissor_h);
}

void retro_gpu_target::upload_texture(const uint32_t *rgba_pixels, int width, int height)
{
	if (!m_valid)
		return;

	scoped_context ctx(m_display, m_surface, m_context);
	if (!ctx.active)
		return;

	g_gl.ActiveTexture(GL_TEXTURE0);
	g_gl.BindTexture(GL_TEXTURE_2D, m_vram_tex);
	g_gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba_pixels);
	g_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	g_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	m_vram_tex_width = width;
	m_vram_tex_height = height;
}

void retro_gpu_target::set_blend_mode(osd::gpu_blend_mode blend)
{
	if (blend == m_current_blend)
		return;
	m_current_blend = blend;

	if (blend == osd::gpu_blend_mode::NONE)
	{
		g_gl.Disable(GL_BLEND);
		return;
	}

	g_gl.Enable(GL_BLEND);
	switch (blend)
	{
	case osd::gpu_blend_mode::HALF_ADD:
		g_gl.BlendEquation(GL_FUNC_ADD);
		g_gl.BlendFunc(GL_CONSTANT_ALPHA, GL_CONSTANT_ALPHA);
		g_gl.BlendColor(0.5f, 0.5f, 0.5f, 0.5f);
		break;
	case osd::gpu_blend_mode::ADD:
		g_gl.BlendEquation(GL_FUNC_ADD);
		g_gl.BlendFunc(GL_ONE, GL_ONE);
		break;
	case osd::gpu_blend_mode::SUBTRACT:
		g_gl.BlendEquation(GL_FUNC_REVERSE_SUBTRACT);
		g_gl.BlendFunc(GL_ONE, GL_ONE);
		break;
	case osd::gpu_blend_mode::ADD_QUARTER:
		g_gl.BlendEquation(GL_FUNC_ADD);
		g_gl.BlendFunc(GL_CONSTANT_ALPHA, GL_ONE);
		g_gl.BlendColor(0.25f, 0.25f, 0.25f, 0.25f);
		break;
	default:
		break;
	}
}

void retro_gpu_target::submit_triangle(const osd::gpu_vertex tri[3], bool textured, osd::gpu_blend_mode blend)
{
	if (!m_valid)
		return;

	scoped_context ctx(m_display, m_surface, m_context);
	if (!ctx.active)
		return;

	set_blend_mode(blend);
	g_gl.Uniform1i(m_u_textured_loc, textured ? 1 : 0);
	g_gl.BindBuffer(GL_ARRAY_BUFFER, m_vbo);
	g_gl.BufferData(GL_ARRAY_BUFFER, sizeof(osd::gpu_vertex) * 3, tri, GL_DYNAMIC_DRAW);
	g_gl.DrawArrays(GL_TRIANGLES, 0, 3);
}

void retro_gpu_target::end_frame_and_readback(uint32_t *rgba_out)
{
	if (!m_valid)
		return;

	scoped_context ctx(m_display, m_surface, m_context);
	if (!ctx.active)
		return;

	g_gl.Finish();

	// glReadPixels is itself affected by the scissor box - disable it for
	// the readback (we always want the whole target back) and restore
	// afterward, since the draw area/clip rect is meant to persist across
	// frames like real PS1 GPU state, not reset here.
	if (m_scissor_enabled)
		g_gl.Disable(GL_SCISSOR_TEST);

	std::vector<uint32_t> tmp(static_cast<size_t>(m_fbo_width) * m_fbo_height);
	// GL_BGRA (not GL_RGBA): MAME's bitmap_rgb32/rgb_t stores pixels as
	// 0x00RRGGBB, i.e. bytes B,G,R,x from low to high on this little-endian
	// host - GL_BGRA's memory byte order matches that directly, avoiding a
	// manual per-pixel channel swap on every readback.
	g_gl.ReadPixels(0, 0, m_fbo_width, m_fbo_height, GL_BGRA, GL_UNSIGNED_BYTE, tmp.data());

	if (m_scissor_enabled)
	{
		g_gl.Enable(GL_SCISSOR_TEST);
		g_gl.Scissor(m_scissor_x, m_scissor_y, m_scissor_w, m_scissor_h);
	}

	// glReadPixels gives row 0 = bottom of the image; flip so rgba_out's
	// row 0 = top, matching bitmap_rgb32/CPU image convention (see
	// interface/gpurender.h).
	for (int y = 0; y < m_fbo_height; y++)
	{
		const uint32_t *src_row = &tmp[static_cast<size_t>(m_fbo_height - 1 - y) * m_fbo_width];
		uint32_t *dst_row = &rgba_out[static_cast<size_t>(y) * m_fbo_width];
		memcpy(dst_row, src_row, static_cast<size_t>(m_fbo_width) * sizeof(uint32_t));
	}
}
