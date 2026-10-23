// license:BSD-3-Clause
// copyright-holders:TrickoUK
/***************************************************************************

    retro_gl_loader.cpp

    See retro_gl_loader.h.

***************************************************************************/
#include "retro_gl_loader.h"

#include <dlfcn.h>
#include <cstdio>

namespace retro_gl {

egl_dispatch g_egl;
gl_dispatch g_gl;
bool g_dispatch_loaded = false;
static bool g_dispatch_load_failed = false;

static bool load_egl_dispatch()
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

#define LOAD_GL(name) g_gl.name = (decltype(g_gl.name))g_egl.GetProcAddress("gl" #name); if (!g_gl.name) { fprintf(stderr, "[retro_gl] missing GL proc: gl%s\n", #name); return false; }
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
	LOAD_GL(ReadPixels); LOAD_GL(PixelStorei); LOAD_GL(Finish); LOAD_GL(Enable); LOAD_GL(Disable); LOAD_GL(Scissor);
	LOAD_GL(BlitFramebuffer);
	LOAD_GL(GenRenderbuffers); LOAD_GL(BindRenderbuffer);
	LOAD_GL(RenderbufferStorageMultisample); LOAD_GL(FramebufferRenderbuffer);
	LOAD_GL(DepthFunc); LOAD_GL(DepthMask); LOAD_GL(ClearDepth); LOAD_GL(ColorMask);
	LOAD_GL(TexSubImage2D); LOAD_GL(VertexAttribIPointer);
	LOAD_GL(DeleteTextures); LOAD_GL(DeleteBuffers); LOAD_GL(DeleteProgram); LOAD_GL(DeleteShader);
	LOAD_GL(DeleteFramebuffers); LOAD_GL(DeleteRenderbuffers); LOAD_GL(DeleteVertexArrays);
	LOAD_GL(Uniform1f); LOAD_GL(Uniform2i); LOAD_GL(GetIntegerv);
	LOAD_GL(GenQueries); LOAD_GL(DeleteQueries); LOAD_GL(BeginQuery); LOAD_GL(EndQuery);
	LOAD_GL(GetQueryObjectui64v); LOAD_GL(GetQueryObjectiv); LOAD_GL(Flush); LOAD_GL(Uniform1iv);
	LOAD_GL(FenceSync); LOAD_GL(ClientWaitSync); LOAD_GL(DeleteSync); LOAD_GL(MapBufferRange); LOAD_GL(UnmapBuffer);
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
		fprintf(stderr, "[retro_gl] failed to dlopen libEGL/libGL - GPU rendering unavailable\n");
		g_dispatch_load_failed = true;
		return false;
	}
	g_dispatch_loaded = true;
	return true;
}

} // namespace retro_gl
