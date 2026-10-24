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

// Links the shaders `vertex` and `fragment` (files in `folder`, named without their extension). Its
// textures are on the units in the order of `samplers` in the fragment shader's own names.
GlRenderer::Program GlRenderer::loadProgram(const std::filesystem::path& folder, const char* vertex, const char* fragment) {
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
    p.frame = gl_.GetUniformLocation(p.id, "type_Frame");
    p.material = gl_.GetUniformLocation(p.id, "type_Material");
    p.screen = gl_.GetUniformLocation(p.id, "type_Screen");
    return p;
}

void GlRenderer::makePrograms(const std::filesystem::path& folder) {
    // The scene's textures, in the order of the HLSL's registers.
    scene_ = loadProgram(folder, "scene.vert", "scene.frag");
    useProgram(scene_);
    const char* sceneSamplers[] = {"baseTexture", "lightMapTexture", "normalTexture", "incidenceTexture", "lightMapFromTexture", "envCubeTexture"};
    for (int unit = 0; unit < 6; ++unit) {
        const GLint at = gl_.GetUniformLocation(scene_.id, sceneSamplers[unit]);
        if (at >= 0) {
            gl_.Uniform1i(at, unit);
        }
    }
    ui_ = loadProgram(folder, "ui.vert", "ui.frag");
    useProgram(ui_);
    if (const GLint at = gl_.GetUniformLocation(ui_.id, "image"); at >= 0) {
        gl_.Uniform1i(at, 0);
    }
}

} // namespace ts2::render
