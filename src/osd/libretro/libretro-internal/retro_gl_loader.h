// license:BSD-3-Clause
// copyright-holders:TrickoUK
/***************************************************************************

    retro_gl_loader.h

    Runtime-loaded EGL/GL entry points shared by the libretro OSD's private
    GPU backends (retro_gpu_engine).

    EGL and GL are loaded entirely via dlopen()/dlsym() rather than linked
    at build time: the build host has no EGL/GL -devel packages, and
    dlopen lets the feature degrade to "unavailable" on any system without
    a working OpenGL/EGL install. For the same reason no <EGL/egl.h> or
    <GL/gl.h> is used; only the types, constants and entry points actually
    needed are declared here.

***************************************************************************/
#ifndef MAME_OSD_LIBRETRO_RETRO_GL_LOADER_H
#define MAME_OSD_LIBRETRO_RETRO_GL_LOADER_H

#pragma once

#include <cstddef>
#include <cstdint>

typedef void *EGLDisplay;
typedef void *EGLContext;
typedef void *EGLSurface;

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
#define GL_PACK_ROW_LENGTH 0x0D02
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
typedef void (*PFNGLUNIFORM1IVPROC)(GLint, GLsizei, const GLint *);
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
typedef void (*PFNGLPIXELSTOREIPROC)(GLenum, GLint);
typedef void (*PFNGLENABLEPROC)(GLenum);
typedef void (*PFNGLDISABLEPROC)(GLenum);
typedef void (*PFNGLSCISSORPROC)(GLint, GLint, GLsizei, GLsizei);
typedef void (*PFNGLBLITFRAMEBUFFERPROC)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
typedef void (*PFNGLGENRENDERBUFFERSPROC)(GLsizei, GLuint *);
typedef void (*PFNGLBINDRENDERBUFFERPROC)(GLenum, GLuint);
typedef void (*PFNGLRENDERBUFFERSTORAGEMULTISAMPLEPROC)(GLenum, GLsizei, GLenum, GLsizei, GLsizei);
typedef void (*PFNGLTEXIMAGE2DMULTISAMPLEPROC)(GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLboolean);
typedef void (*PFNGLFRAMEBUFFERRENDERBUFFERPROC)(GLenum, GLenum, GLenum, GLuint);
typedef const GLchar *(*PFNGLGETSTRINGPROC)(GLenum);
typedef GLenum (*PFNGLGETERRORPROC)(void);
typedef void (*PFNGLDEPTHFUNCPROC)(GLenum);
typedef void (*PFNGLDEPTHMASKPROC)(GLboolean);
typedef void (*PFNGLCLEARDEPTHPROC)(double);
typedef void (*PFNGLCOLORMASKPROC)(GLboolean, GLboolean, GLboolean, GLboolean);
typedef void (*PFNGLTEXSUBIMAGE2DPROC)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *);
typedef void (*PFNGLVERTEXATTRIBIPOINTERPROC)(GLuint, GLint, GLenum, GLsizei, const void *);
typedef void (*PFNGLDELETETEXTURESPROC)(GLsizei, const GLuint *);
typedef void (*PFNGLDELETEBUFFERSPROC)(GLsizei, const GLuint *);
typedef void (*PFNGLDELETEPROGRAMPROC)(GLuint);
typedef void (*PFNGLDELETESHADERPROC)(GLuint);
typedef void (*PFNGLDELETEFRAMEBUFFERSPROC)(GLsizei, const GLuint *);
typedef void (*PFNGLDELETERENDERBUFFERSPROC)(GLsizei, const GLuint *);
typedef void (*PFNGLDELETEVERTEXARRAYSPROC)(GLsizei, const GLuint *);
typedef void (*PFNGLUNIFORM1FPROC)(GLint, GLfloat);
typedef void (*PFNGLUNIFORM2IPROC)(GLint, GLint, GLint);
typedef void (*PFNGLGETINTEGERVPROC)(GLenum, GLint *);
typedef void (*PFNGLGENQUERIESPROC)(GLsizei, GLuint *);
typedef void (*PFNGLDELETEQUERIESPROC)(GLsizei, const GLuint *);
typedef void (*PFNGLBEGINQUERYPROC)(GLenum, GLuint);
typedef void (*PFNGLENDQUERYPROC)(GLenum);
typedef void (*PFNGLGETQUERYOBJECTUI64VPROC)(GLuint, GLenum, uint64_t *);
typedef void (*PFNGLGETQUERYOBJECTIVPROC)(GLuint, GLenum, GLint *);
typedef void (*PFNGLFLUSHPROC)(void);
typedef void *GLsync;
typedef ptrdiff_t GLintptr;
typedef uint64_t GLuint64;
typedef GLsync (*PFNGLFENCESYNCPROC)(GLenum, GLbitfield);
typedef GLenum (*PFNGLCLIENTWAITSYNCPROC)(GLsync, GLbitfield, GLuint64);
typedef void (*PFNGLDELETESYNCPROC)(GLsync);
typedef void *(*PFNGLMAPBUFFERRANGEPROC)(GLenum, GLintptr, GLsizeiptr, GLbitfield);
typedef GLboolean (*PFNGLUNMAPBUFFERPROC)(GLenum);
#define GL_VENDOR 0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
#define GL_DEPTH_BUFFER_BIT 0x00000100
#define GL_STENCIL_BUFFER_BIT 0x00000400
#define GL_DEPTH_TEST 0x0B71
#define GL_DEPTH_COMPONENT24 0x81A6
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_NEVER 0x0200
#define GL_LESS 0x0201
#define GL_EQUAL 0x0202
#define GL_LEQUAL 0x0203
#define GL_GREATER 0x0204
#define GL_NOTEQUAL 0x0205
#define GL_GEQUAL 0x0206
#define GL_ALWAYS 0x0207
#define GL_UNPACK_ROW_LENGTH 0x0CF2
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_PACK_ALIGNMENT 0x0D05
#define GL_R8UI 0x8232
#define GL_R32UI 0x8236
#define GL_RG32UI 0x823C
#define GL_RGBA32I 0x8D82
#define GL_RG_INTEGER 0x8228
#define GL_RGBA_INTEGER 0x8D99
#define GL_UNSIGNED_INT 0x1405
#define GL_INT 0x1404
#define GL_LINEAR 0x2601
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_SRC_COLOR 0x0300
#define GL_ONE_MINUS_SRC_COLOR 0x0301
#define GL_DST_COLOR 0x0306
#define GL_ONE_MINUS_DST_COLOR 0x0307
#define GL_SRC_ALPHA_SATURATE 0x0308
#define GL_ONE_MINUS_CONSTANT_ALPHA 0x8004
#define GL_MAX_TEXTURE_SIZE 0x0D33
#define GL_MAX_SAMPLES 0x8D57
#define GL_MAX_COLOR_TEXTURE_SAMPLES 0x910E
#define GL_MULTISAMPLE 0x809D
#define GL_TEXTURE_2D_MULTISAMPLE 0x9100
#define GL_DEPTH_WRITEMASK 0x0B72
#define GL_STATIC_DRAW 0x88E4
#define GL_STREAM_DRAW 0x88E0
#define GL_NO_ERROR 0
#define GL_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAA
#define GL_TIME_ELAPSED 0x88BF
#define GL_PIXEL_PACK_BUFFER 0x88EB
#define GL_STREAM_READ 0x88E1
#define GL_SYNC_GPU_COMMANDS_COMPLETE 0x9117
#define GL_SYNC_FLUSH_COMMANDS_BIT 0x00000001
#define GL_MAP_READ_BIT 0x0001
#define GL_ALREADY_SIGNALED 0x911A
#define GL_TIMEOUT_EXPIRED 0x911B
#define GL_CONDITION_SATISFIED 0x911C
#define GL_WAIT_FAILED 0x911D
#define GL_RG 0x8227
#define GL_QUERY_RESULT 0x8866
#define GL_QUERY_RESULT_AVAILABLE 0x8867


namespace retro_gl {

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
	PFNGLUNIFORM1IVPROC Uniform1iv = nullptr;
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
	PFNGLPIXELSTOREIPROC PixelStorei = nullptr;
	PFNGLENABLEPROC Enable = nullptr;
	PFNGLDISABLEPROC Disable = nullptr;
	PFNGLSCISSORPROC Scissor = nullptr;
	PFNGLBLITFRAMEBUFFERPROC BlitFramebuffer = nullptr;
	PFNGLGENRENDERBUFFERSPROC GenRenderbuffers = nullptr;
	PFNGLBINDRENDERBUFFERPROC BindRenderbuffer = nullptr;
	PFNGLRENDERBUFFERSTORAGEMULTISAMPLEPROC RenderbufferStorageMultisample = nullptr;
	PFNGLTEXIMAGE2DMULTISAMPLEPROC TexImage2DMultisample = nullptr;
	PFNGLFRAMEBUFFERRENDERBUFFERPROC FramebufferRenderbuffer = nullptr;
	PFNGLGETSTRINGPROC GetString = nullptr;
	PFNGLGETERRORPROC GetError = nullptr;
	PFNGLDEPTHFUNCPROC DepthFunc = nullptr;
	PFNGLDEPTHMASKPROC DepthMask = nullptr;
	PFNGLCLEARDEPTHPROC ClearDepth = nullptr;
	PFNGLCOLORMASKPROC ColorMask = nullptr;
	PFNGLTEXSUBIMAGE2DPROC TexSubImage2D = nullptr;
	PFNGLVERTEXATTRIBIPOINTERPROC VertexAttribIPointer = nullptr;
	PFNGLDELETETEXTURESPROC DeleteTextures = nullptr;
	PFNGLDELETEBUFFERSPROC DeleteBuffers = nullptr;
	PFNGLDELETEPROGRAMPROC DeleteProgram = nullptr;
	PFNGLDELETESHADERPROC DeleteShader = nullptr;
	PFNGLDELETEFRAMEBUFFERSPROC DeleteFramebuffers = nullptr;
	PFNGLDELETERENDERBUFFERSPROC DeleteRenderbuffers = nullptr;
	PFNGLDELETEVERTEXARRAYSPROC DeleteVertexArrays = nullptr;
	PFNGLUNIFORM1FPROC Uniform1f = nullptr;
	PFNGLUNIFORM2IPROC Uniform2i = nullptr;
	PFNGLGETINTEGERVPROC GetIntegerv = nullptr;
	PFNGLGENQUERIESPROC GenQueries = nullptr;
	PFNGLDELETEQUERIESPROC DeleteQueries = nullptr;
	PFNGLBEGINQUERYPROC BeginQuery = nullptr;
	PFNGLENDQUERYPROC EndQuery = nullptr;
	PFNGLGETQUERYOBJECTUI64VPROC GetQueryObjectui64v = nullptr;
	PFNGLGETQUERYOBJECTIVPROC GetQueryObjectiv = nullptr;
	PFNGLFLUSHPROC Flush = nullptr;
	PFNGLFENCESYNCPROC FenceSync = nullptr;
	PFNGLCLIENTWAITSYNCPROC ClientWaitSync = nullptr;
	PFNGLDELETESYNCPROC DeleteSync = nullptr;
	PFNGLMAPBUFFERRANGEPROC MapBufferRange = nullptr;
	PFNGLUNMAPBUFFERPROC UnmapBuffer = nullptr;
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

extern egl_dispatch g_egl;
extern gl_dispatch g_gl;
extern bool g_dispatch_loaded;

// load libEGL/libGL once; false if either is missing
bool ensure_dispatch_loaded();
// resolve the GL entry points; needs a current context
bool load_gl_dispatch();

} // namespace retro_gl

#endif // MAME_OSD_LIBRETRO_RETRO_GL_LOADER_H
