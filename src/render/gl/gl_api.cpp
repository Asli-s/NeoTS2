#include "render/gl/gl_api.hpp"

#include <SDL3/SDL.h>

#include <cstdlib>
#include <cstring>
#include <string>

namespace ts2::render::gl {

bool Api::load(const char*& missing) {
    missing = nullptr;
#define TS2_GL_LOAD(name)                                                                \
    name = reinterpret_cast<decltype(name)>(SDL_GL_GetProcAddress("gl" #name));          \
    if (name == nullptr && missing == nullptr) {                                         \
        missing = "gl" #name;                                                            \
    }
    TS2_GL_REQUIRED(TS2_GL_LOAD)
#undef TS2_GL_LOAD
    if (missing != nullptr) {
        return false;
    }
    const auto* version = reinterpret_cast<const char*>(GetString(GL_VERSION));
    if (version != nullptr) {
        majorVersion = std::atoi(version);
        const char* dot = std::strchr(version, '.');
        minorVersion = dot ? std::atoi(dot + 1) : 0;
    }
    GetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTextureSize);

    // Framebuffer objects: the EXT extension, or the same functions without the suffix that GL 3.0 and
    // GL_ARB_framebuffer_object have (their constants are the same).
    const bool ext = hasExtension("GL_EXT_framebuffer_object");
    const bool core = majorVersion >= 3 || hasExtension("GL_ARB_framebuffer_object");
    framebuffers = ext || core;
    if (framebuffers) {
#define TS2_GL_LOAD_OPTIONAL(name)                                                                       \
    {                                                                                                    \
        std::string function = "gl" #name;                                                               \
        if (!ext) {                                                                                      \
            function.resize(function.size() - 3);                                                        \
        }                                                                                                \
        name = reinterpret_cast<decltype(name)>(SDL_GL_GetProcAddress(function.c_str()));                \
        framebuffers = framebuffers && name != nullptr;                                                  \
    }
        TS2_GL_FRAMEBUFFER(TS2_GL_LOAD_OPTIONAL)
#undef TS2_GL_LOAD_OPTIONAL
    }
    anisotropy = hasExtension("GL_EXT_texture_filter_anisotropic");
    if (anisotropy) {
        GetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &maxAnisotropy);
    }
    return true;
}

bool Api::hasExtension(const char* name) const {
    const auto* list = reinterpret_cast<const char*>(GetString(GL_EXTENSIONS));
    if (list == nullptr) {
        return false;
    }
    const std::size_t length = std::strlen(name);
    for (const char* at = list; (at = std::strstr(at, name)) != nullptr; at += length) {
        const bool starts = at == list || at[-1] == ' ';
        const bool ends = at[length] == ' ' || at[length] == '\0';
        if (starts && ends) {
            return true;
        }
    }
    return false;
}

} // namespace ts2::render::gl
