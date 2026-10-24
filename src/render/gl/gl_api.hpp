// The OpenGL functions the GL renderer calls, loaded through SDL so that one build runs on any
// system with a GL 2.1 driver (and on Mac OS X's OpenGL framework, which has no loader of its own).
#pragma once

#include "platform/opengl.hpp"

namespace ts2::render::gl {

// GL 1.1 to 1.5, which both ways of drawing require: textures, buffers and drawing from them.
#define TS2_GL_REQUIRED(X)                                                                                              \
    X(Clear) X(ClearColor) X(ClearDepth) X(Viewport) X(Enable) X(Disable) X(DepthFunc) X(DepthMask) X(DepthRange)       \
    X(ColorMask) X(CullFace) X(FrontFace) X(BlendFunc) X(Flush) X(Finish) X(GetError) X(GetString) X(GetIntegerv) X(GetFloatv)       \
    X(PixelStorei) X(ReadPixels) X(GenTextures) X(DeleteTextures) X(BindTexture) X(TexImage2D) X(TexSubImage2D)         \
    X(TexParameteri) X(TexParameterf) X(DrawElements) X(DrawArrays) X(ActiveTexture) X(BlendFuncSeparate)              \
    X(GenBuffers) X(DeleteBuffers) X(BindBuffer) X(BufferData) X(BufferSubData) X(Scissor)

// GL 2.0 shaders, for drawing with GLSL.
#define TS2_GL_SHADERS(X)                                                                                               \
    X(CreateShader) X(DeleteShader) X(ShaderSource) X(CompileShader) X(GetShaderiv) X(GetShaderInfoLog)                 \
    X(CreateProgram) X(DeleteProgram) X(AttachShader) X(BindAttribLocation) X(LinkProgram) X(GetProgramiv)              \
    X(GetProgramInfoLog) X(UseProgram) X(GetUniformLocation) X(Uniform1i) X(Uniform1f) X(Uniform2f) X(Uniform4fv)       \
    X(EnableVertexAttribArray) X(DisableVertexAttribArray) X(VertexAttribPointer)

// The fixed pipeline, for drawing without shaders: matrices, vertex arrays, texture environment and alpha test.
#define TS2_GL_FIXED(X)                                                                                                 \
    X(MatrixMode) X(LoadMatrixf) X(LoadIdentity) X(EnableClientState) X(DisableClientState) X(VertexPointer)            \
    X(ColorPointer) X(TexCoordPointer) X(ClientActiveTexture) X(TexEnvi) X(AlphaFunc) X(Color4f)

// Optional: framebuffer objects, for drawing off-screen.
#define TS2_GL_FRAMEBUFFER(X)                                                                                           \
    X(GenFramebuffersEXT) X(DeleteFramebuffersEXT) X(BindFramebufferEXT) X(FramebufferTexture2DEXT)                     \
    X(CheckFramebufferStatusEXT) X(GenRenderbuffersEXT) X(DeleteRenderbuffersEXT) X(BindRenderbufferEXT)                \
    X(RenderbufferStorageEXT) X(FramebufferRenderbufferEXT)

struct Api {
#define TS2_GL_MEMBER(name) decltype(&::gl##name) name = nullptr;
    TS2_GL_REQUIRED(TS2_GL_MEMBER)
    TS2_GL_SHADERS(TS2_GL_MEMBER)
    TS2_GL_FIXED(TS2_GL_MEMBER)
    TS2_GL_FRAMEBUFFER(TS2_GL_MEMBER)
#undef TS2_GL_MEMBER

    // Loads the functions for the current context: the shaders' when it draws with GLSL, else the
    // fixed pipeline's. False, with `missing` naming the first one not found, when a required one is absent.
    bool load(const char*& missing, bool fixedFunction = false);
    // Whether the context lists an extension.
    bool hasExtension(const char* name) const;

    bool framebuffers = false;   // GL_EXT_framebuffer_object (or the core GL 3.0 version of it)
    bool anisotropy = false;     // GL_EXT_texture_filter_anisotropic
    float maxAnisotropy = 1;
    int majorVersion = 0, minorVersion = 0;
    GLint maxTextureSize = 1024;  // GLint, not int: on Mac OS X before 10.6 it is a long, and glGetIntegerv wants its own type
};

} // namespace ts2::render::gl
