#include "render/gl/gl_renderer.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace ts2::render {

GLuint GlRenderer::compile(GLenum stage, const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        throw std::runtime_error("can't read " + file.string());
    }
    std::ostringstream text;
    text << in.rdbuf();
    const std::string source = text.str();
    const GLuint shader = gl_.CreateShader(stage);
    const char* lines[] = {source.c_str()};
    gl_.ShaderSource(shader, 1, lines, nullptr);
    gl_.CompileShader(shader);
    GLint ok = 0;
    gl_.GetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048] = {};
        gl_.GetShaderInfoLog(shader, sizeof(log) - 1, nullptr, log);
        gl_.DeleteShader(shader);
        throw std::runtime_error(file.filename().string() + ": " + log);
    }
    return shader;
}

// Links the shaders `vertex` and `fragment` (files in `folder`, named without their extension). The
// shaders' constants are the blocks of that name (see the shaders' cbuffers), and the fragment
// shader's textures are on the units in the order of `samplers`.
GlRenderer::Program GlRenderer::loadProgram(const std::filesystem::path& folder, const char* vertex, const char* fragment,
                                            const char* vertexBlock, const char* fragmentBlock,
                                            std::initializer_list<const char*> samplers) {
    const GLuint vs = compile(GL_VERTEX_SHADER, folder / (std::string(vertex) + ".glsl"));
    GLuint fs = 0;
    try {
        fs = compile(GL_FRAGMENT_SHADER, folder / (std::string(fragment) + ".glsl"));
    } catch (...) {
        gl_.DeleteShader(vs);
        throw;
    }
    Program p;
    p.id = gl_.CreateProgram();
    gl_.AttachShader(p.id, vs);
    gl_.AttachShader(p.id, fs);
    // The vertex inputs are the attribute locations the HLSL gave them (LOCATION(n) : TEXCOORDn).
    for (GLuint location = 0; location < 12; ++location) {
        gl_.BindAttribLocation(p.id, location, ("in_var_TEXCOORD" + std::to_string(location)).c_str());
    }
    gl_.LinkProgram(p.id);
    gl_.DeleteShader(vs);
    gl_.DeleteShader(fs);
    GLint ok = 0;
    gl_.GetProgramiv(p.id, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048] = {};
        gl_.GetProgramInfoLog(p.id, sizeof(log) - 1, nullptr, log);
        gl_.DeleteProgram(p.id);
        throw std::runtime_error(std::string(vertex) + " + " + fragment + ": " + log);
    }
    if (vertexBlock != nullptr) {
        p.vertex = gl_.GetUniformLocation(p.id, vertexBlock);
    }
    if (fragmentBlock != nullptr) {
        p.fragment = gl_.GetUniformLocation(p.id, fragmentBlock);
    }
    useProgram(p);
    GLint unit = 0;
    for (const char* name : samplers) {
        if (const GLint at = gl_.GetUniformLocation(p.id, name); at >= 0) {
            gl_.Uniform1i(at, unit);
        }
        ++unit;
    }
    return p;
}

void GlRenderer::makePrograms(const std::filesystem::path& folder) {
    // The scene's textures, in the order of the HLSL's registers.
    scene_ = loadProgram(folder, "scene.vert", "scene.frag", "type_Frame", "type_Material",
                         {"baseTexture", "lightMapTexture", "normalTexture", "incidenceTexture", "lightMapFromTexture", "envCubeTexture"});
    ui_ = loadProgram(folder, "ui.vert", "ui.frag", "type_Screen", nullptr, {"image"});
    terrain_ = loadProgram(folder, "terrain.vert", "terrain.frag", "type_Frame", "type_Terrain",
                           {"paint", "alphas0", "alphas1", "cells0", "cells1", "lightMap", "gradient"});
    road_ = loadProgram(folder, "road.vert", "road.frag", "type_Frame", "type_Road", {"roads", "lightMap"});
    imposter_ = loadProgram(folder, "imposter.vert", "imposter.frag", "type_Frame", "type_Part", {"image"});
    sky_ = loadProgram(folder, "sky.vert", "sky.frag", nullptr, "type_Camera", {"sky"});
    clip_ = loadProgram(folder, "clip.vert", "clip.frag", "type_Frame", nullptr, {});
    water_ = loadProgram(folder, "water.vert", "water.frag", "type_Frame", "type_Water", {"reflection", "ripples"});
}

} // namespace ts2::render
