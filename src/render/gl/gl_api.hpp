// The OpenGL functions the GL 2 renderer calls, loaded through SDL so that one build runs on any
// system with a GL 2.1 driver (and on Mac OS X's OpenGL framework, which has no loader of its own).
#pragma once

#define GL_GLEXT_PROTOTYPES 1
#include <SDL3/SDL_opengl.h>

namespace ts2::render::gl {

// GL 1.1 and 1.2 to 2.1, which the renderer requires.
#define TS2_GL_REQUIRED(X)                                                                                              \
    X(Clear) X(ClearColor) X(ClearDepth) X(Viewport) X(Enable) X(Disable) X(DepthFunc) X(DepthMask) X(DepthRange)       \
    X(ColorMask) X(CullFace) X(FrontFace) X(BlendFunc) X(Flush) X(Finish) X(GetError) X(GetString) X(GetIntegerv) X(GetFloatv)       \
    X(PixelStorei) X(ReadPixels) X(GenTextures) X(DeleteTextures) X(BindTexture) X(TexImage2D) X(TexSubImage2D)         \
    X(TexParameteri) X(TexParameterf) X(DrawElements) X(DrawArrays) X(ActiveTexture) X(BlendFuncSeparate)              \
    X(GenBuffers) X(DeleteBuffers) X(BindBuffer) X(BufferData) X(BufferSubData) X(CreateShader) X(DeleteShader)         \
    X(ShaderSource) X(CompileShader) X(GetShaderiv) X(GetShaderInfoLog) X(CreateProgram) X(DeleteProgram)              \
    X(AttachShader) X(BindAttribLocation) X(LinkProgram) X(GetProgramiv) X(GetProgramInfoLog) X(UseProgram)             \
    X(GetUniformLocation) X(Uniform1i) X(Uniform1f) X(Uniform2f) X(Uniform4fv) X(EnableVertexAttribArray)               \
    X(DisableVertexAttribArray) X(VertexAttribPointer) X(Scissor)

// Optional: framebuffer objects, for drawing off-screen.
#define TS2_GL_FRAMEBUFFER(X)                                                                                           \
    X(GenFramebuffersEXT) X(DeleteFramebuffersEXT) X(BindFramebufferEXT) X(FramebufferTexture2DEXT)                     \
    X(CheckFramebufferStatusEXT) X(GenRenderbuffersEXT) X(DeleteRenderbuffersEXT) X(BindRenderbufferEXT)                \
    X(RenderbufferStorageEXT) X(FramebufferRenderbufferEXT)

struct Api {
#define TS2_GL_MEMBER(name) decltype(&::gl##name) name = nullptr;
    TS2_GL_REQUIRED(TS2_GL_MEMBER)
    TS2_GL_FRAMEBUFFER(TS2_GL_MEMBER)
#undef TS2_GL_MEMBER

    // Loads the functions for the current context. False, with `missing` naming the first one not
    // found, when a required one is absent.
    bool load(const char*& missing);
    // Whether the context lists an extension.
    bool hasExtension(const char* name) const;

    bool framebuffers = false;   // GL_EXT_framebuffer_object (or the core GL 3.0 version of it)
    bool anisotropy = false;     // GL_EXT_texture_filter_anisotropic
    float maxAnisotropy = 1;
    int majorVersion = 0, minorVersion = 0;
    int maxTextureSize = 1024;
};

} // namespace ts2::render::gl
