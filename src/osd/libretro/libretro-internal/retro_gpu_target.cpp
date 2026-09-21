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
#define GL_UNSIGNED_SHORT 0x1403
#define GL_FLOAT 0x1406
#define GL_RGBA 0x1908
#define GL_BGRA 0x80E1
#define GL_RGBA8 0x8058
#define GL_RED_INTEGER 0x8D94
#define GL_R16UI 0x8234
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_NEAREST 0x2600
#define GL_TEXTURE0 0x84C0
#define GL_FRAMEBUFFER 0x8D40
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_RENDERBUFFER 0x8D41
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
typedef void (*PFNGLBLITFRAMEBUFFERPROC)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
typedef void (*PFNGLGENRENDERBUFFERSPROC)(GLsizei, GLuint *);
typedef void (*PFNGLBINDRENDERBUFFERPROC)(GLenum, GLuint);
typedef void (*PFNGLRENDERBUFFERSTORAGEMULTISAMPLEPROC)(GLenum, GLsizei, GLenum, GLsizei, GLsizei);
typedef void (*PFNGLFRAMEBUFFERRENDERBUFFERPROC)(GLenum, GLenum, GLenum, GLuint);
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
	PFNGLBLITFRAMEBUFFERPROC BlitFramebuffer = nullptr;
	PFNGLGENRENDERBUFFERSPROC GenRenderbuffers = nullptr;
	PFNGLBINDRENDERBUFFERPROC BindRenderbuffer = nullptr;
	PFNGLRENDERBUFFERSTORAGEMULTISAMPLEPROC RenderbufferStorageMultisample = nullptr;
	PFNGLFRAMEBUFFERRENDERBUFFERPROC FramebufferRenderbuffer = nullptr;
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
	LOAD_GL(BlitFramebuffer);
	LOAD_GL(GenRenderbuffers); LOAD_GL(BindRenderbuffer);
	LOAD_GL(RenderbufferStorageMultisample); LOAD_GL(FramebufferRenderbuffer);
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

/* a_w (osd::gpu_vertex::w) is a perspective divisor for v_color/v_uv
 * interpolation only - it never moves the final screen position. Standard
 * "keep position, correct interpolation" trick: emitting clip.xyz =
 * ndc*w, clip.w = w means the GPU's own perspective divide (clip.xyz /
 * clip.w) recovers exactly ndc.xy unchanged ((ndc*w)/w == ndc), while
 * every varying is interpolated using OpenGL's standard perspective-
 * correct rule (weighted by 1/clip.w per vertex) instead of the flat/
 * affine interpolation a constant w=1 produces. a_w defaults to 1.0 for
 * every vertex without real PGXP depth data (see gpurender.h), which
 * makes this bit-for-bit identical to the previous hardcoded-w=1.0
 * behavior (original PS1-accurate affine warping) for anything that
 * isn't PGXP-corrected. */
const char *vertex_shader_src =
	"#version 330 core\n"
	"layout(location=0) in vec2 a_pos;\n"
	"layout(location=1) in vec4 a_color;\n"
	"layout(location=2) in vec2 a_uv;\n"
	"layout(location=3) in float a_w;\n"
	"layout(location=4) in vec4 a_uv_clamp;\n"
	"uniform vec2 u_target_size;\n"
	"out vec4 v_color;\n"
	"out vec2 v_uv;\n"
	"out vec4 v_uv_clamp;\n"
	"void main() {\n"
	"    v_color = a_color;\n"
	"    v_uv = a_uv;\n"
	"    v_uv_clamp = a_uv_clamp;\n"
	"    float ndc_x = (a_pos.x / u_target_size.x) * 2.0 - 1.0;\n"
	"    float ndc_y = 1.0 - (a_pos.y / u_target_size.y) * 2.0;\n"
	"    gl_Position = vec4(ndc_x * a_w, ndc_y * a_w, 0.0, a_w);\n"
	"}\n";

/* Textured mode treats v_color as a PS1-style modulation factor (0.5 = the
 * neutral/no-op shade PS1 games use for "raw" unshaded textures - see
 * psxgpu_device::gpu_submit_flat_textured_polygon()) rather than an alpha
 * multiplier, matching the CPU rasterizer's SHADEDPIXEL/TRANSPARENTPIXEL
 * macros (psx.cpp) which multiply the texel by the vertex shade centered
 * at 0x80/128.
 *
 * Texture decode (CLUT lookup / 4bpp-8bpp-16bpp unpack) happens here, not
 * on the CPU: u_tex holds the device's *entire* raw VRAM as undecoded
 * 16-bit texels (see retro_gpu_target::upload_vram()), and u_tx/u_ty/
 * u_tp/u_clutx/u_cluty (set per polygon batch by set_texture_page())
 * select where in it to sample from - the exact same addressing as psx.cpp's
 * TEXTURE4BIT/8BIT/15BIT macros, reproduced bit-for-bit so results match
 * the software path. This replaced an earlier design that CPU-decoded a
 * full 256x256 RGBA page per texture-page switch - profiling found that
 * decode was 43% of total CPU time (dominant even over PS1 CPU emulation
 * itself) because consecutive polygons in real content switch texture
 * pages far more often than they reuse one, so a small last-page cache
 * barely helped. Moving the decode into the GPU removes that cost
 * entirely; a zero-valued texel (n_bgr == 0) is the PS1 GPU's
 * "transparent, don't draw" marker and is discarded rather than blended,
 * exactly as the CPU path's n_bgr == 0 check does. */
/* u_texfilter: 0 = nearest (bit-exact with the original PS1-accurate
 * unfiltered lookup below), 1 = bilinear, 2 = trilinear.
 *
 * decode_texel() is the same page/CLUT addressing as before, just factored
 * out so it can be called at multiple integer (u,v) taps for filtering.
 * bgr == 0u ("transparent, don't draw") is carried through as alpha 0
 * instead of an unconditional discard, so a filtered tap that lands off
 * the edge of a sprite can be excluded from the weighted blend rather than
 * always killing the whole fragment - each filter function renormalizes
 * over only the taps that hit real texel data, and only discards if every
 * tap it sampled was transparent.
 *
 * Neither filter mode has any per-object knowledge of a sprite/polygon's
 * own UV rect (only raw texel addresses reach this shader), so a bilinear
 * tap can still cross into an unrelated image packed into the same 256x256
 * VRAM texture page - the well-known "edge bleeding" artifact shared by
 * every other PS1 HLE renderer's texture filtering (e.g. Beetle PSX HW).
 * Not fixed here; see CLAUDE.md if revisiting.
 *
 * Trilinear has no real mip chain to sample (PS1 has none, and the CLUT
 * decode happens per-fragment from raw VRAM, not a precomputed RGBA
 * texture that could have mipmaps generated for it). Instead it blends
 * the normal bilinear (level 0) sample with a coarser, box-filtered
 * sample taken on a texel grid spaced 2 apart (an approximate "level 1"),
 * weighted by how minified the polygon is in screen space (via
 * dFdx/dFdy(v_uv)) - this smooths shimmering on receding/distant polygons
 * similarly to real trilinear filtering without requiring precomputed
 * mip levels. */
const char *fragment_shader_src =
	"#version 330 core\n"
	"in vec4 v_color;\n"
	"in vec2 v_uv;\n"
	"in vec4 v_uv_clamp;\n"
	"uniform usampler2D u_tex;\n"
	"uniform int u_textured;\n"
	"uniform int u_texfilter;\n"
	"uniform int u_stp_mode;\n"
	"uniform int u_tp;\n"
	"uniform int u_tx;\n"
	"uniform int u_ty;\n"
	"uniform int u_clutx;\n"
	"uniform int u_cluty;\n"
	"uniform int u_vram_height;\n"
	"out vec4 frag_color;\n"
	"uint fetch_bgr(int u, int v) {\n"
	"    int row = (u_ty + v) % u_vram_height;\n"
	"    int clutrow = u_cluty % u_vram_height;\n"
	"    uint bgr;\n"
	"    if (u_tp == 0) {\n"
	"        int col = (u_tx + (u >> 2)) & 1023;\n"
	"        uint word = texelFetch(u_tex, ivec2(col, row), 0).r;\n"
	"        uint idx = (word >> uint((u & 3) << 2)) & 0x0Fu;\n"
	"        int clutcol = (u_clutx + int(idx)) & 1023;\n"
	"        bgr = texelFetch(u_tex, ivec2(clutcol, clutrow), 0).r;\n"
	"    } else if (u_tp == 1) {\n"
	"        int col = (u_tx + (u >> 1)) & 1023;\n"
	"        uint word = texelFetch(u_tex, ivec2(col, row), 0).r;\n"
	"        uint idx = (word >> uint((u & 1) << 3)) & 0xFFu;\n"
	"        int clutcol = (u_clutx + int(idx)) & 1023;\n"
	"        bgr = texelFetch(u_tex, ivec2(clutcol, clutrow), 0).r;\n"
	"    } else {\n"
	"        int col = (u_tx + u) & 1023;\n"
	"        bgr = texelFetch(u_tex, ivec2(col, row), 0).r;\n"
	"    }\n"
	"    return bgr;\n"
	"}\n"
	"vec4 decode_texel(int u, int v) {\n"
	"    uint bgr = fetch_bgr(u, v);\n"
	"    if (bgr == 0u) return vec4(0.0);\n"
	"    uint r8 = (bgr & 0x1Fu) << 3;\n"
	"    uint g8 = ((bgr >> 5) & 0x1Fu) << 3;\n"
	"    uint b8 = ((bgr >> 10) & 0x1Fu) << 3;\n"
	"    return vec4(vec3(float(r8), float(g8), float(b8)) / 255.0, 1.0);\n"
	"}\n"
	// Clamping each tap's integer coordinate to the source primitive's own
	// UV footprint (v_uv_clamp, set per-vertex from psx.cpp - see gpu_vertex
	// in gpurender.h) before decoding it is what prevents a bilinear/
	// trilinear sample near a texture's edge from pulling in an unrelated
	// texture packed next to it in the same shared VRAM page - the classic
	// PS1 HLE "texture bleeding" artifact (clamp-to-edge instead, same
	// technique other PS1 HLE renderers with texture filtering use, e.g.
	// Beetle PSX HW).
	"vec4 sample_bilinear(vec2 uv, int step, ivec2 cmin, ivec2 cmax) {\n"
	"    vec2 guv = uv / float(step) - 0.5;\n"
	"    ivec2 base = ivec2(floor(guv)) * step;\n"
	"    vec2 frac = fract(guv);\n"
	"    ivec2 t00 = clamp(base, cmin, cmax);\n"
	"    ivec2 t10 = clamp(base + ivec2(step, 0), cmin, cmax);\n"
	"    ivec2 t01 = clamp(base + ivec2(0, step), cmin, cmax);\n"
	"    ivec2 t11 = clamp(base + ivec2(step, step), cmin, cmax);\n"
	"    vec4 c00 = decode_texel(t00.x, t00.y);\n"
	"    vec4 c10 = decode_texel(t10.x, t10.y);\n"
	"    vec4 c01 = decode_texel(t01.x, t01.y);\n"
	"    vec4 c11 = decode_texel(t11.x, t11.y);\n"
	"    float w00 = (1.0 - frac.x) * (1.0 - frac.y) * c00.a;\n"
	"    float w10 = frac.x * (1.0 - frac.y) * c10.a;\n"
	"    float w01 = (1.0 - frac.x) * frac.y * c01.a;\n"
	"    float w11 = frac.x * frac.y * c11.a;\n"
	"    float wsum = w00 + w10 + w01 + w11;\n"
	"    if (wsum <= 0.0) return vec4(0.0);\n"
	"    return vec4((c00.rgb * w00 + c10.rgb * w10 + c01.rgb * w01 + c11.rgb * w11) / wsum, 1.0);\n"
	"}\n"
	// N64-style "3-point" filtering (as offered by e.g. Beetle PSX HW's
	// texture filtering option): instead of bilinear's blend across all 4
	// texels in the cell, split the cell along its diagonal into two
	// triangles by which side frac.x+frac.y falls on, and barycentric-blend
	// only the 3 texels of *that* triangle. Same edge-clamp/transparency-
	// renormalize handling as sample_bilinear(), just a different weighting
	// scheme - this is a style choice (matches real low-precision hardware
	// like the N64's own filtering more closely than true bilinear), not a
	// fix for anything bilinear/trilinear get wrong.
	"vec4 sample_3point(vec2 uv, ivec2 cmin, ivec2 cmax) {\n"
	"    vec2 guv = uv - 0.5;\n"
	"    ivec2 base = ivec2(floor(guv));\n"
	"    vec2 frac = fract(guv);\n"
	"    ivec2 t00 = clamp(base, cmin, cmax);\n"
	"    ivec2 t10 = clamp(base + ivec2(1, 0), cmin, cmax);\n"
	"    ivec2 t01 = clamp(base + ivec2(0, 1), cmin, cmax);\n"
	"    ivec2 t11 = clamp(base + ivec2(1, 1), cmin, cmax);\n"
	"    vec4 c00 = decode_texel(t00.x, t00.y);\n"
	"    vec4 c10 = decode_texel(t10.x, t10.y);\n"
	"    vec4 c01 = decode_texel(t01.x, t01.y);\n"
	"    vec4 c11 = decode_texel(t11.x, t11.y);\n"
	"    float w00, w10, w01, w11;\n"
	"    if (frac.x + frac.y <= 1.0) {\n"
	"        w00 = 1.0 - frac.x - frac.y; w10 = frac.x; w01 = frac.y; w11 = 0.0;\n"
	"    } else {\n"
	"        w11 = frac.x + frac.y - 1.0; w10 = 1.0 - frac.y; w01 = 1.0 - frac.x; w00 = 0.0;\n"
	"    }\n"
	"    w00 *= c00.a; w10 *= c10.a; w01 *= c01.a; w11 *= c11.a;\n"
	"    float wsum = w00 + w10 + w01 + w11;\n"
	"    if (wsum <= 0.0) return vec4(0.0);\n"
	"    return vec4((c00.rgb * w00 + c10.rgb * w10 + c01.rgb * w01 + c11.rgb * w11) / wsum, 1.0);\n"
	"}\n"
	// Per-texel semi-transparency (STP, texel bit 15) decision for the
	// fragment, ported from Beetle PSX HW's command_fragment.glsl.h (its
	// get_texel_bilinear()/get_texel_3point() plus the
	// `is_texel_semi_transparent != draw_semi_transparent` test in main()).
	// Unfiltered: the nearest texel's own bit. Filtered: the colour is a
	// blend of a small tap footprint, so - exactly like Beetle - the STP
	// bit is interpolated with the *same weights* as the colour (a
	// transparent tap, bgr == 0, contributes STP 0 and opacity 0 at full
	// weight, no renormalisation) and rounded at 0.5, and a fragment whose
	// interpolated opacity is below 0.5 is dropped. Deciding from the
	// single nearest texel instead misclassified edge fragments and drew a
	// hard opaque outline around translucent sprites.
	"void texel_stp(out bool stp, out float opacity) {\n"
	"    if (u_texfilter == 0) {\n"
	"        stp = (fetch_bgr(int(v_uv.x), int(v_uv.y)) & 0x8000u) != 0u;\n"
	"        opacity = 1.0;\n"
	"        return;\n"
	"    }\n"
	"    ivec2 cmin = ivec2(floor(v_uv_clamp.xy));\n"
	"    ivec2 cmax = ivec2(floor(v_uv_clamp.zw));\n"
	"    vec2 guv = v_uv - 0.5;\n"
	"    ivec2 base = ivec2(floor(guv));\n"
	"    vec2 fr = fract(guv);\n"
	"    ivec2 t00 = clamp(base, cmin, cmax);\n"
	"    ivec2 t10 = clamp(base + ivec2(1, 0), cmin, cmax);\n"
	"    ivec2 t01 = clamp(base + ivec2(0, 1), cmin, cmax);\n"
	"    ivec2 t11 = clamp(base + ivec2(1, 1), cmin, cmax);\n"
	"    uint b00 = fetch_bgr(t00.x, t00.y);\n"
	"    uint b10 = fetch_bgr(t10.x, t10.y);\n"
	"    uint b01 = fetch_bgr(t01.x, t01.y);\n"
	"    uint b11 = fetch_bgr(t11.x, t11.y);\n"
	"    float w00, w10, w01, w11;\n"
	"    if (u_texfilter == 3) {\n"
	"        if (fr.x + fr.y <= 1.0) {\n"
	"            w00 = 1.0 - fr.x - fr.y; w10 = fr.x; w01 = fr.y; w11 = 0.0;\n"
	"        } else {\n"
	"            w11 = fr.x + fr.y - 1.0; w10 = 1.0 - fr.y; w01 = 1.0 - fr.x; w00 = 0.0;\n"
	"        }\n"
	"    } else {\n"
	"        w00 = (1.0 - fr.x) * (1.0 - fr.y); w10 = fr.x * (1.0 - fr.y);\n"
	"        w01 = (1.0 - fr.x) * fr.y; w11 = fr.x * fr.y;\n"
	"    }\n"
	"    opacity = w00 * (b00 != 0u ? 1.0 : 0.0) + w10 * (b10 != 0u ? 1.0 : 0.0)\n"
	"            + w01 * (b01 != 0u ? 1.0 : 0.0) + w11 * (b11 != 0u ? 1.0 : 0.0);\n"
	"    float sv = w00 * float((b00 >> 15) & 1u) + w10 * float((b10 >> 15) & 1u)\n"
	"             + w01 * float((b01 >> 15) & 1u) + w11 * float((b11 >> 15) & 1u);\n"
	"    stp = floor(sv + 0.5) >= 1.0;\n"
	"}\n"
	"void main() {\n"
	"    if (u_textured != 0) {\n"
	"        vec4 texel;\n"
	"        if (u_texfilter == 0) {\n"
	"            texel = decode_texel(int(v_uv.x), int(v_uv.y));\n"
	"        } else if (u_texfilter == 1) {\n"
	"            ivec2 cmin = ivec2(floor(v_uv_clamp.xy));\n"
	"            ivec2 cmax = ivec2(floor(v_uv_clamp.zw));\n"
	"            texel = sample_bilinear(v_uv, 1, cmin, cmax);\n"
	"        } else if (u_texfilter == 2) {\n"
	"            ivec2 cmin = ivec2(floor(v_uv_clamp.xy));\n"
	"            ivec2 cmax = ivec2(floor(v_uv_clamp.zw));\n"
	"            vec4 lvl0 = sample_bilinear(v_uv, 1, cmin, cmax);\n"
	"            vec4 lvl1 = sample_bilinear(v_uv, 2, cmin, cmax);\n"
	"            float density = max(length(dFdx(v_uv)), length(dFdy(v_uv)));\n"
	"            float t = clamp(density - 1.0, 0.0, 1.0);\n"
	"            if (lvl0.a <= 0.0) { texel = lvl1; }\n"
	"            else if (lvl1.a <= 0.0) { texel = lvl0; }\n"
	"            else { texel = vec4(mix(lvl0.rgb, lvl1.rgb, t), 1.0); }\n"
	"        } else {\n"
	"            ivec2 cmin = ivec2(floor(v_uv_clamp.xy));\n"
	"            ivec2 cmax = ivec2(floor(v_uv_clamp.zw));\n"
	"            texel = sample_3point(v_uv, cmin, cmax);\n"
	"        }\n"
	"        if (texel.a <= 0.0) discard;\n"
	"        if (u_stp_mode != 0) {\n"
	"            bool stp; float stp_opacity;\n"
	"            texel_stp(stp, stp_opacity);\n"
	"            if (stp_opacity < 0.5) discard;\n"
	"            if (u_stp_mode == 1 && !stp) discard;\n"
	"            if (u_stp_mode == 2 && stp) discard;\n"
	"        }\n"
	"        frag_color = vec4(clamp(texel.rgb * (v_color.rgb * 2.0), 0.0, 1.0), 1.0);\n"
	"    } else {\n"
	"        frag_color = v_color;\n"
	"    }\n"
	"}\n";

// Makes the target's EGL context current for the lifetime of this object,
// restoring whatever was current on the calling thread beforehand when it
// goes out of scope. Used individually inside every public entry point
// below - see the header comment on m_batch_active for why: RetroArch's
// frontend can perform its own EGL work synchronously from inside a MAME
// core call, and holding our context across *multiple calls with control
// returning to MAME/RetroArch's scheduler in between* was found to
// eventually corrupt shared driver state, even with careful lazy
// reacquisition. A per-call eglMakeCurrent pair is too slow for a real
// polygon-heavy scene on its own, so when the caller has already
// acquired the context itself via begin_batch() (a tight, synchronous
// run of calls with nothing interleaved - safe for the same reason a
// single call is safe), pass already_current=true to skip the redundant
// acquire/release entirely.
struct scoped_context
{
	EGLDisplay saved_display;
	EGLSurface saved_draw_surface;
	EGLSurface saved_read_surface;
	EGLContext saved_context;
	bool active;
	bool owns_switch;

	scoped_context(EGLDisplay display, EGLSurface surface, EGLContext context, bool already_current)
	{
		if (already_current)
		{
			active = true;
			owns_switch = false;
			return;
		}
		saved_display = g_egl.GetCurrentDisplay();
		saved_draw_surface = g_egl.GetCurrentSurface(EGL_DRAW_);
		saved_read_surface = g_egl.GetCurrentSurface(EGL_READ_);
		saved_context = g_egl.GetCurrentContext();
		active = g_egl.MakeCurrent(display, surface, surface, context);
		owns_switch = active;
		if (!active)
			fprintf(stderr, "[retro_gpu_target] scoped_context: eglMakeCurrent failed: 0x%x\n", g_egl.GetError());
	}

	~scoped_context()
	{
		if (owns_switch)
			g_egl.MakeCurrent(saved_display, saved_draw_surface, saved_read_surface, saved_context);
	}

	scoped_context(const scoped_context &) = delete;
	scoped_context &operator=(const scoped_context &) = delete;
};

} // anonymous namespace

retro_gpu_target::retro_gpu_target(int msaa_samples, int texfilter_mode)
	: m_display(nullptr), m_context(nullptr), m_surface(nullptr), m_valid(false)
	, m_batch_active(false)
	, m_batch_saved_display(nullptr), m_batch_saved_draw_surface(nullptr), m_batch_saved_read_surface(nullptr), m_batch_saved_context(nullptr)
	, m_msaa_samples(msaa_samples < 0 ? 0 : msaa_samples)
	, m_fbo(0), m_color_rb_ms(0), m_resolve_fbo(0), m_color_tex(0), m_fbo_width(0), m_fbo_height(0)
	, m_vram_tex(0), m_vram_tex_width(0), m_vram_tex_height(0)
	, m_program(0), m_vao(0), m_vbo(0)
	, m_u_target_size_loc(-1), m_u_textured_loc(-1)
	, m_u_tp_loc(-1), m_u_tx_loc(-1), m_u_ty_loc(-1), m_u_clutx_loc(-1), m_u_cluty_loc(-1), m_u_vram_height_loc(-1)
	, m_u_texfilter_loc(-1), m_u_stp_mode_loc(-1)
	, m_texfilter_mode(texfilter_mode < 0 || texfilter_mode > 3 ? 0 : texfilter_mode)
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
	m_u_tp_loc = g_gl.GetUniformLocation(m_program, "u_tp");
	m_u_tx_loc = g_gl.GetUniformLocation(m_program, "u_tx");
	m_u_ty_loc = g_gl.GetUniformLocation(m_program, "u_ty");
	m_u_clutx_loc = g_gl.GetUniformLocation(m_program, "u_clutx");
	m_u_cluty_loc = g_gl.GetUniformLocation(m_program, "u_cluty");
	m_u_vram_height_loc = g_gl.GetUniformLocation(m_program, "u_vram_height");
	m_u_texfilter_loc = g_gl.GetUniformLocation(m_program, "u_texfilter");
	m_u_stp_mode_loc = g_gl.GetUniformLocation(m_program, "u_stp_mode");
	GLint tex_loc = g_gl.GetUniformLocation(m_program, "u_tex");
	g_gl.Uniform1i(tex_loc, 0);
	// Just a safe initial default - submit_triangle(s) resolves the real
	// per-draw value (option mode, or forced nearest for non-filterable
	// draws) every call, see there.
	g_gl.Uniform1i(m_u_texfilter_loc, m_texfilter_mode);

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
	g_gl.EnableVertexAttribArray(3);
	g_gl.VertexAttribPointer(3, 1, GL_FLOAT, 0, sizeof(osd::gpu_vertex), (void*)offsetof(osd::gpu_vertex, w));
	g_gl.EnableVertexAttribArray(4);
	g_gl.VertexAttribPointer(4, 4, GL_FLOAT, 0, sizeof(osd::gpu_vertex), (void*)offsetof(osd::gpu_vertex, u_min));

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
	if (!m_color_rb_ms)
		g_gl.GenRenderbuffers(1, &m_color_rb_ms);
	if (!m_resolve_fbo)
		g_gl.GenFramebuffers(1, &m_resolve_fbo);
	if (!m_color_tex)
		g_gl.GenTextures(1, &m_color_tex);

	// The actual multisample render target - everything submit_triangle(s)/
	// copy_rect() draws into all frame.
	g_gl.BindFramebuffer(GL_FRAMEBUFFER, m_fbo);
	g_gl.BindRenderbuffer(GL_RENDERBUFFER, m_color_rb_ms);
	// m_msaa_samples: caller-selected (mame_psx_gpu_msaa core option, 4x
	// when enabled), 0 disables MSAA - a 0-sample renderbuffer is
	// spec-legal and equivalent to a plain single-sample one, well within
	// what this host's GL 4.6 core profile supports either way - no
	// MAX_SAMPLES capability query needed for this target.
	g_gl.RenderbufferStorageMultisample(GL_RENDERBUFFER, m_msaa_samples, GL_RGBA8, width, height);
	g_gl.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, m_color_rb_ms);

	// The single-sample resolve target - glReadPixels can't read a
	// multisample framebuffer directly, so end_frame_and_readback() blits
	// m_fbo into this one (resolving the samples) right before reading it
	// back. Never drawn into directly - only ever a glBlitFramebuffer
	// destination.
	g_gl.BindFramebuffer(GL_FRAMEBUFFER, m_resolve_fbo);
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
	g_gl.BindFramebuffer(GL_FRAMEBUFFER, m_fbo);
	g_gl.ClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	g_gl.Clear(GL_COLOR_BUFFER_BIT);

	m_fbo_width = width;
	m_fbo_height = height;
}

void retro_gpu_target::begin_frame(int width, int height)
{
	if (!m_valid)
		return;

	scoped_context ctx(m_display, m_surface, m_context, m_batch_active);
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

void retro_gpu_target::begin_batch()
{
	if (!m_valid || m_batch_active)
		return;
	m_batch_saved_display = g_egl.GetCurrentDisplay();
	m_batch_saved_draw_surface = g_egl.GetCurrentSurface(EGL_DRAW_);
	m_batch_saved_read_surface = g_egl.GetCurrentSurface(EGL_READ_);
	m_batch_saved_context = g_egl.GetCurrentContext();
	if (g_egl.MakeCurrent(m_display, m_surface, m_surface, m_context))
		m_batch_active = true;
	else
		fprintf(stderr, "[retro_gpu_target] begin_batch: eglMakeCurrent failed: 0x%x\n", g_egl.GetError());
}

void retro_gpu_target::end_batch()
{
	if (!m_batch_active)
		return;
	g_egl.MakeCurrent(m_batch_saved_display, m_batch_saved_draw_surface, m_batch_saved_read_surface, m_batch_saved_context);
	m_batch_active = false;
}

void retro_gpu_target::set_clip_rect(int x1, int y1, int x2, int y2)
{
	if (!m_valid)
		return;

	scoped_context ctx(m_display, m_surface, m_context, m_batch_active);
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

// PS1 GPU VRAM-to-VRAM copy commands (MoveImage) bypass the draw-area clip
// entirely - matches set_clip_rect()'s own top-left-origin -> GL bottom-up
// y-flip so a copy lands at the same target pixels a same-rect draw would.
// Blitting the FBO onto itself (rather than sourcing from the raw VRAM
// texture upload_vram() provides) is deliberate: the raw VRAM texture only
// reflects primitives that still write to the CPU-side p_vram buffer
// (persistent 2D image data, lines, dots), not the polygon/rectangle/sprite
// primitives now rendered GPU-side only - this target's own color buffer is
// the only place with correct, up-to-date pixels for those.
void retro_gpu_target::copy_rect(int sx, int sy, int dx, int dy, int w, int h)
{
	if (!m_valid || w <= 0 || h <= 0)
		return;

	scoped_context ctx(m_display, m_surface, m_context, m_batch_active);
	if (!ctx.active)
		return;

	bool scissor_was_enabled = m_scissor_enabled;
	if (scissor_was_enabled)
		g_gl.Disable(GL_SCISSOR_TEST);

	int src_x0 = sx, src_x1 = sx + w;
	int src_y0 = m_fbo_height - (sy + h), src_y1 = m_fbo_height - sy;
	int dst_x0 = dx, dst_x1 = dx + w;
	int dst_y0 = m_fbo_height - (dy + h), dst_y1 = m_fbo_height - dy;

	g_gl.BindFramebuffer(GL_READ_FRAMEBUFFER, m_fbo);
	g_gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, m_fbo);
	g_gl.BlitFramebuffer(src_x0, src_y0, src_x1, src_y1, dst_x0, dst_y0, dst_x1, dst_y1, GL_COLOR_BUFFER_BIT, GL_NEAREST);
	g_gl.BindFramebuffer(GL_FRAMEBUFFER, m_fbo);

	if (scissor_was_enabled)
		g_gl.Enable(GL_SCISSOR_TEST);
}

void retro_gpu_target::upload_vram(const uint16_t *vram_words, int width, int height)
{
	if (!m_valid)
		return;

	scoped_context ctx(m_display, m_surface, m_context, m_batch_active);
	if (!ctx.active)
		return;

	// Raw, undecoded upload - one 16-bit texel per VRAM word, GL_R16UI so
	// the fragment shader can texelFetch() it as an integer and do its own
	// CLUT/bit-unpack decode (see fragment_shader_src) instead of this being
	// pre-decoded to RGBA8 on the CPU. Called once per frame regardless of
	// how many texture pages/CLUTs polygons in that frame actually use -
	// see set_texture_page() for the per-polygon addressing into this.
	g_gl.ActiveTexture(GL_TEXTURE0);
	g_gl.BindTexture(GL_TEXTURE_2D, m_vram_tex);
	g_gl.TexImage2D(GL_TEXTURE_2D, 0, GL_R16UI, width, height, 0, GL_RED_INTEGER, GL_UNSIGNED_SHORT, vram_words);
	g_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	g_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	m_vram_tex_width = width;
	m_vram_tex_height = height;

	g_gl.UseProgram(m_program);
	g_gl.Uniform1i(m_u_vram_height_loc, height);
}

void retro_gpu_target::set_texture_page(int tx, int ty, int tp, int clutx, int cluty)
{
	if (!m_valid)
		return;

	scoped_context ctx(m_display, m_surface, m_context, m_batch_active);
	if (!ctx.active)
		return;

	g_gl.UseProgram(m_program);
	g_gl.Uniform1i(m_u_tp_loc, tp);
	g_gl.Uniform1i(m_u_tx_loc, tx);
	g_gl.Uniform1i(m_u_ty_loc, ty);
	g_gl.Uniform1i(m_u_clutx_loc, clutx);
	g_gl.Uniform1i(m_u_cluty_loc, cluty);
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

void retro_gpu_target::submit_triangle(const osd::gpu_vertex tri[3], bool textured, osd::gpu_blend_mode blend, bool filterable)
{
	submit_triangles(tri, 3, textured, blend, filterable);
}

void retro_gpu_target::submit_triangles(const osd::gpu_vertex *verts, int count, bool textured, osd::gpu_blend_mode blend, bool filterable)
{
	if (!m_valid || count <= 0)
		return;

	scoped_context ctx(m_display, m_surface, m_context, m_batch_active);
	if (!ctx.active)
		return;

	g_gl.Uniform1i(m_u_textured_loc, textured ? 1 : 0);
	// Resolve the user's texfilter option against this draw's eligibility
	// (see gpurender.h's filterable doc comment) - falls back to nearest
	// (0) for draws the caller marked as not filterable, regardless of the
	// option, so 2D/pixel-art content stays crisp even with bilinear or
	// trilinear filtering turned on for 3D geometry.
	g_gl.Uniform1i(m_u_texfilter_loc, filterable ? m_texfilter_mode : 0);
	g_gl.BindBuffer(GL_ARRAY_BUFFER, m_vbo);
	g_gl.BufferData(GL_ARRAY_BUFFER, sizeof(osd::gpu_vertex) * count, verts, GL_DYNAMIC_DRAW);

	if (textured && blend != osd::gpu_blend_mode::NONE)
	{
		// Per-texel semi-transparency: on the PS1, a semi-transparent
		// *textured* primitive only blends texels whose own bit 15 (the
		// "STP" bit) is set - texels with it clear are drawn fully opaque
		// (psx.cpp's TRANSPARENTPIXEL macro). One fixed-function blend
		// mode per draw call can't express that split (and reverse-
		// subtract can't express "opaque" at all), so draw the batch
		// twice: pass 1 = only STP-set texels, with the requested blend;
		// pass 2 = only STP-clear texels, blending off. Overlapping
		// primitives inside one batch are consequently reordered
		// blended-first, opaque-second - only visible where semi-
		// transparent polygons overlap each other within a single
		// same-state run, which is rare.
		set_blend_mode(blend);
		g_gl.Uniform1i(m_u_stp_mode_loc, 1);
		g_gl.DrawArrays(GL_TRIANGLES, 0, count);
		set_blend_mode(osd::gpu_blend_mode::NONE);
		g_gl.Uniform1i(m_u_stp_mode_loc, 2);
		g_gl.DrawArrays(GL_TRIANGLES, 0, count);
		g_gl.Uniform1i(m_u_stp_mode_loc, 0);
		return;
	}

	set_blend_mode(blend);
	g_gl.DrawArrays(GL_TRIANGLES, 0, count);
}

void retro_gpu_target::end_frame_and_readback(uint32_t *rgba_out)
{
	if (!m_valid)
		return;

	scoped_context ctx(m_display, m_surface, m_context, m_batch_active);
	if (!ctx.active)
		return;

	g_gl.Finish();

	// glReadPixels (and the resolve blit below) are affected by the
	// scissor box - disable it for both (we always want the whole target
	// back) and restore afterward, since the draw area/clip rect is meant
	// to persist across frames like real PS1 GPU state, not reset here.
	if (m_scissor_enabled)
		g_gl.Disable(GL_SCISSOR_TEST);

	// m_fbo is multisampled (see m_msaa_samples/resize_target()) - glReadPixels
	// can't read a multisample framebuffer directly, so resolve it into the
	// single-sample m_resolve_fbo first. This is the only place per frame
	// the samples actually get resolved - copy_rect() mid-frame operates on
	// m_fbo directly and stays multisampled the rest of the time.
	g_gl.BindFramebuffer(GL_READ_FRAMEBUFFER, m_fbo);
	g_gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, m_resolve_fbo);
	g_gl.BlitFramebuffer(0, 0, m_fbo_width, m_fbo_height, 0, 0, m_fbo_width, m_fbo_height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
	g_gl.BindFramebuffer(GL_READ_FRAMEBUFFER, m_resolve_fbo);

	std::vector<uint32_t> tmp(static_cast<size_t>(m_fbo_width) * m_fbo_height);
	// GL_BGRA (not GL_RGBA): MAME's bitmap_rgb32/rgb_t stores pixels as
	// 0x00RRGGBB, i.e. bytes B,G,R,x from low to high on this little-endian
	// host - GL_BGRA's memory byte order matches that directly, avoiding a
	// manual per-pixel channel swap on every readback.
	g_gl.ReadPixels(0, 0, m_fbo_width, m_fbo_height, GL_BGRA, GL_UNSIGNED_BYTE, tmp.data());

	// Leave GL_FRAMEBUFFER bound to m_fbo (the real render target) again,
	// matching what every other entry point (submit_triangle(s), copy_rect(),
	// the next begin_frame()) expects to already be bound.
	g_gl.BindFramebuffer(GL_FRAMEBUFFER, m_fbo);

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
