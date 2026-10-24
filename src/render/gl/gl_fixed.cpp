// Drawing without shaders: a lot's objects and the UI, with the fixed pipeline of OpenGL 1.5. The game's
// lighting is worked out for each vertex on the CPU (see cpu_light.hpp) and goes in the vertex colour,
// which the texture then multiplies. A lot's floors and walls, lit by their light map, draw their
// texture and then multiply the light map in with a second pass, so a draw never needs two texture units.
#include "render/cpu_light.hpp"
#include "render/fixed_layout.hpp"
#include "render/gl/gl_renderer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace ts2::render {

namespace {

constexpr GLsizei kVertexBytes = static_cast<GLsizei>(kVertexFloats * sizeof(float));
constexpr unsigned kVertexArray = 1, kColorArray = 2, kTexCoordArray = 4;
// Where a vertex keeps what (see kVertexFloats).
constexpr std::size_t kNormalAt = 3, kUvAt = 6, kAlphaAt = 8, kLightUvAt = 9, kColorAt = 11;
// The light a surface that a light map would darken is drawn with where it can't be multiplied in
// (see drawListFixed), as a share of the light map's gain.
constexpr float kBlendedLightMapShare = 0.5f;

const void* offset(std::size_t bytes) {
    return reinterpret_cast<const void*>(bytes);
}

Mat4 fromRows(const float* rows) {
    Mat4 r;
    std::copy_n(rows, 16, r.m.begin());
    return r;
}

// OpenGL takes matrices by columns, ours are by rows.
std::array<float, 16> byColumns(const Mat4& m) {
    std::array<float, 16> r{};
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            r[static_cast<std::size_t>(column * 4 + row)] = m.m[static_cast<std::size_t>(row * 4 + column)];
        }
    }
    return r;
}

// The shaders put depth from 0 to w and OpenGL wants it from -w to w, which the GLSL made from them is
// fixed up for in the same way (z' = 2z - w). Depth still runs from 1 at the near plane to 0 at the far one.
Mat4 clipSpace() {
    Mat4 r;
    r.m = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 2, -1, 0, 0, 0, 1};
    return r;
}

std::uint8_t byteOf(float v) {
    return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255));
}

// What colours a draw's vertices to what scene.frag.hlsl makes of them, apart from the texture the
// colour is multiplied by. `lightMapShare` is the share of a light map's gain that draws its surface
// when the light map itself can't be multiplied in.
void vertexColors(const VertexUniforms& vu, const FragmentUniforms& fu, const float* vertices, unsigned count, float lightMapShare,
                  std::vector<std::uint8_t>& out) {
    const Mat4 model = fromRows(vu.model);
    const std::array<float, 16>& m = model.m;
    const bool lightMap = fu.lightMap[0] > 0;
    const bool hasTexture = fu.hasTexture > 0.5f;
    const bool emissive = fu.emissive[3] > 0.5f;
    const float* color = fu.color;
    out.resize(std::size_t{count} * 4);
    for (unsigned i = 0; i < count; ++i) {
        const float* v = vertices + std::size_t{i} * kVertexFloats;
        const float* vc = v + kColorAt;
        float rgb[3];
        if (lightMap) {
            // Floors and walls: the texture alone or times the material's colour, then the light map.
            for (std::size_t k = 0; k < 3; ++k) {
                rgb[k] = (fu.lightMap[1] > 0.5f && hasTexture ? 1.0f : color[k] * vc[k]) * lightMapShare;
            }
        } else if (fu.unlit > 0.5f) {
            for (std::size_t k = 0; k < 3; ++k) {
                rgb[k] = color[k] * vc[k];
            }
        } else {
            const Vec3 world{m[0] * v[0] + m[1] * v[1] + m[2] * v[2] + m[3], m[4] * v[0] + m[5] * v[1] + m[6] * v[2] + m[7],
                             m[8] * v[0] + m[9] * v[1] + m[10] * v[2] + m[11]};
            const Vec3 normal{m[0] * v[kNormalAt] + m[1] * v[kNormalAt + 1] + m[2] * v[kNormalAt + 2],
                              m[4] * v[kNormalAt] + m[5] * v[kNormalAt + 1] + m[6] * v[kNormalAt + 2],
                              m[8] * v[kNormalAt] + m[9] * v[kNormalAt + 1] + m[10] * v[kNormalAt + 2]};
            // Without normal maps the lights they would take are lit here with the others.
            const std::array<float, 4> light = gameLight(vu, normal, world, true);
            if (light[3] > 1.5f) {  // a family thumbnail: the texture times the light, which holds the material's diffuse
                for (std::size_t k = 0; k < 3; ++k) {
                    rgb[k] = (hasTexture ? 1.0f : color[k]) * light[k];
                }
            } else if (light[3] > 0.5f) {
                for (std::size_t k = 0; k < 3; ++k) {
                    // A Sim: the vertex light times the diffuse scaled by 0.8; others: the material's colour times the light.
                    rgb[k] = fu.simLighting > 0.5f ? (hasTexture ? 1.0f : color[k]) * vc[k] * light[k] * color[k] * 0.8f
                                                   : color[k] * vc[k] * light[k];
                }
            } else {  // a sky and a sun
                const Vec3 n = normalize(normal);
                const float sky = 0.45f + (0.85f - 0.45f) * std::clamp(n[2] * 0.5f + 0.5f, 0.0f, 1.0f);
                const float sun = std::clamp(dot(n, {fu.sunDirection[0], fu.sunDirection[1], fu.sunDirection[2]}), 0.0f, 1.0f) * 0.75f;
                for (std::size_t k = 0; k < 3; ++k) {
                    rgb[k] = color[k] * vc[k] * (sky + sun);
                }
            }
            if (emissive) {  // added over the lit colour, times the texture and the vertex colour
                for (std::size_t k = 0; k < 3; ++k) {
                    rgb[k] += vc[k] * fu.emissive[k];
                }
            }
        }
        std::uint8_t* at = &out[std::size_t{i} * 4];
        at[0] = byteOf(rgb[0]);
        at[1] = byteOf(rgb[1]);
        at[2] = byteOf(rgb[2]);
        at[3] = byteOf(color[3] * v[kAlphaAt]);
    }
}

} // namespace

void GlRenderer::fxEnable(GLenum capability) {
    if (subset_) {
        subset_->enable(capability);
    }
    gl_.Enable(capability);
}

void GlRenderer::fxBlendFunc(GLenum srcRgb, GLenum dstRgb, GLenum srcAlpha, GLenum dstAlpha) {
    if (subset_) {
        subset_->blend(srcRgb, dstRgb, srcAlpha, dstAlpha);
    }
    gl_.BlendFuncSeparate(srcRgb, dstRgb, srcAlpha, dstAlpha);
}

void GlRenderer::fxAlphaFunc(GLenum func, GLclampf reference) {
    if (subset_) {
        subset_->alphaFunc(func);
    }
    gl_.AlphaFunc(func, reference);
}

void GlRenderer::fxTexEnv(GLint mode) {
    if (subset_) {
        subset_->texEnv(static_cast<unsigned>(mode));
    }
    gl_.TexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, mode);
}

void GlRenderer::fxDrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices) {
    if (subset_) {
        subset_->drawElements(type);
    }
    gl_.DrawElements(mode, count, type, indices);
}

void GlRenderer::fxVertexPointer(GLint size, GLenum type, GLsizei stride, const void* pointer) {
    if (subset_) {
        subset_->array("Vertex", size, type, stride);
    }
    gl_.VertexPointer(size, type, stride, pointer);
}

void GlRenderer::fxColorPointer(GLint size, GLenum type, GLsizei stride, const void* pointer) {
    if (subset_) {
        subset_->array("Color", size, type, stride);
    }
    gl_.ColorPointer(size, type, stride, pointer);
}

void GlRenderer::fxTexCoordPointer(GLint size, GLenum type, GLsizei stride, const void* pointer) {
    if (subset_) {
        subset_->array("TexCoord", size, type, stride);
    }
    gl_.TexCoordPointer(size, type, stride, pointer);
}

// Enables the vertex arrays in `mask` (kVertexArray, kColorArray, kTexCoordArray) and disables the others.
void GlRenderer::setClientArrays(unsigned mask) {
    const bool unknown = clientArrays_ == ~0u;
    const std::array<std::pair<unsigned, GLenum>, 3> arrays{{{kVertexArray, GL_VERTEX_ARRAY}, {kColorArray, GL_COLOR_ARRAY},
                                                             {kTexCoordArray, GL_TEXTURE_COORD_ARRAY}}};
    for (const auto& [bit, capability] : arrays) {
        const bool want = (mask & bit) != 0, have = !unknown && (clientArrays_ & bit) != 0;
        if (want && (unknown || !have)) {
            gl_.EnableClientState(capability);
        } else if (!want && (unknown || have)) {
            gl_.DisableClientState(capability);
        }
    }
    clientArrays_ = mask;
}

// The objects, as scene.vert.hlsl and scene.frag.hlsl would draw them, less what needs shaders: the
// normal-mapped light, specular and environment cubes. Blended light-mapped surfaces (floor decals)
// can't have the light map multiplied in over what is behind them, so they are darkened by a share of
// its gain instead.
void GlRenderer::drawListFixed(const FrameView& f, const std::vector<Draw>& list, int blend) {
    if (list.empty()) {
        return;
    }
    gl_.ActiveTexture(GL_TEXTURE0);
    fxEnable(GL_TEXTURE_2D);
    fxTexEnv(GL_MODULATE);
    gl_.MatrixMode(GL_PROJECTION);
    gl_.LoadMatrixf(byColumns(clipSpace() * f.viewProjection).data());
    gl_.MatrixMode(GL_MODELVIEW);
    for (const auto& d : list) {
        const MeshBuffers& buffers = gpu_->meshBuffers[d.primitive->buffers];
        const auto& p = *d.primitive;
        if (buffers.vertices == 0 || buffers.indices == 0 || (!p.skinned && buffers.cpuVertices.empty())) {
            continue;
        }
        const scene::Material* m = d.material;
        State state;
        state.blend = blend && m && m->alpha == scene::AlphaMode::Add ? 2 : blend;
        state.cull = !(m && m->doubleSided);
        state.depthWrite = blend == 0;
        setState(state);
        const DrawUniforms u = FramePlan::uniformsFor(*this, *this, f, d, blend);
        const VertexUniforms& vu = u.vertex.frame;
        const FragmentUniforms& fu = u.fragment;

        // The vertices as they are in the model's space: a skinned primitive's are posed (into the streaming buffer).
        const float* vertices = nullptr;
        std::size_t start = 0;
        if (p.skinned) {
            poseSkinned(p, buffers, static_cast<std::size_t>(d.instance - instances_.data()));
            vertices = scratch_.data();
        } else {
            vertices = &buffers.cpuVertices[static_cast<std::size_t>(p.firstVertex) * kVertexFloats];
            start = static_cast<std::size_t>(p.firstVertex) * kVertexBytes;
        }
        const bool lightMapped = fu.lightMap[0] > 0;
        const bool multiplied = lightMapped && blend == 0;
        vertexColors(vu, fu, vertices, p.vertices, lightMapped && !multiplied ? std::min(1.0f, kBlendedLightMapShare * fu.lightMap[0]) : 1.0f,
                     litColors_);

        // The lit colours, then the vertices they belong to.
        const auto colorBytes = static_cast<GLsizeiptr>(litColors_.size());
        gl_.BindBuffer(GL_ARRAY_BUFFER, colorStream_);
        gl_.BufferData(GL_ARRAY_BUFFER, colorBytes, nullptr, GL_STREAM_DRAW);
        gl_.BufferData(GL_ARRAY_BUFFER, colorBytes, litColors_.data(), GL_STREAM_DRAW);
        setClientArrays(kVertexArray | kColorArray | kTexCoordArray);
        fxColorPointer(4, GL_UNSIGNED_BYTE, 0, nullptr);
        arrayBuffer_ = colorStream_;
        const GLuint vertexBuffer = p.skinned ? streamVertices_ : buffers.vertices;
        gl_.BindBuffer(GL_ARRAY_BUFFER, vertexBuffer);
        arrayBuffer_ = vertexBuffer;
        fxVertexPointer(3, GL_FLOAT, kVertexBytes, offset(start));
        fxTexCoordPointer(2, GL_FLOAT, kVertexBytes, offset(start + kUvAt * sizeof(float)));
        if (elementBuffer_ != buffers.indices) {
            gl_.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffers.indices);
            elementBuffer_ = buffers.indices;
        }
        gl_.LoadMatrixf(byColumns(fromRows(vu.model)).data());

        bindTexture(0, textureFor(u.samplers[0].texture), u.samplers[0].sampler == SamplerKind::Clamped ? GL_CLAMP_TO_EDGE : GL_REPEAT);
        // A pixel is discarded below the cutoff. GL_GREATER is the test the oldest drivers all have, so
        // the reference is a shade under the cutoff (alpha is a byte).
        if (fu.alphaCutoff > 0) {
            fxEnable(GL_ALPHA_TEST);
            fxAlphaFunc(GL_GREATER, std::max(0.0f, fu.alphaCutoff - 1.0f / 512));
        } else {
            gl_.Disable(GL_ALPHA_TEST);
        }
        const auto indices = offset(static_cast<std::size_t>(p.firstIndex) * 4);
        fxDrawElements(GL_TRIANGLES, static_cast<GLsizei>(p.count), GL_UNSIGNED_INT, indices);

        if (multiplied) {
            // The light map times what the first pass drew (the gain is the pass's colour), writing no depth.
            gl_.Disable(GL_ALPHA_TEST);
            fxEnable(GL_BLEND);
            fxBlendFunc(GL_DST_COLOR, GL_ZERO, GL_ZERO, GL_ONE);
            gl_.DepthMask(GL_FALSE);
            setClientArrays(kVertexArray | kTexCoordArray);
            const float gain = fu.lightMap[0];
            gl_.Color4f(gain, gain, gain, 1);
            fxTexCoordPointer(2, GL_FLOAT, kVertexBytes, offset(start + kLightUvAt * sizeof(float)));
            bindTexture(0, textureFor(u.samplers[1].texture), GL_CLAMP_TO_EDGE);
            fxDrawElements(GL_TRIANGLES, static_cast<GLsizei>(p.count), GL_UNSIGNED_INT, indices);
            state_.reset();  // the blend and depth mask are not what setState thinks they are
        }
    }
    endFixed();
}

// The UI: pixels from the top left, as ui.vert.hlsl has them.
void GlRenderer::beginFixedSprites(unsigned width, unsigned height) {
    gl_.ActiveTexture(GL_TEXTURE0);
    fxEnable(GL_TEXTURE_2D);
    fxTexEnv(GL_MODULATE);
    gl_.Disable(GL_ALPHA_TEST);
    Mat4 screen;
    screen.m = {2.0f / static_cast<float>(width), 0, 0, -1, 0, -2.0f / static_cast<float>(height), 0, 1, 0, 0, 1, 0, 0, 0, 0, 1};
    gl_.MatrixMode(GL_PROJECTION);
    gl_.LoadMatrixf(byColumns(screen).data());
    gl_.MatrixMode(GL_MODELVIEW);
    gl_.LoadIdentity();
}

// Points the arrays at the sprites' vertices (position, texture coordinates, colour), bound now.
void GlRenderer::fixedSpritePointers(GLsizei stride) {
    setClientArrays(kVertexArray | kColorArray | kTexCoordArray);
    fxVertexPointer(2, GL_FLOAT, stride, offset(0));
    fxTexCoordPointer(2, GL_FLOAT, stride, offset(2 * sizeof(float)));
    fxColorPointer(4, GL_FLOAT, stride, offset(4 * sizeof(float)));
}

void GlRenderer::endFixed() {
    gl_.Disable(GL_ALPHA_TEST);
}

// Turns the second texture unit on (with the texture environment that multiplies it in) or off.
void GlRenderer::fixedTextureUnits(unsigned units) {
    if (units == textureUnits_) {
        return;
    }
    if (subset_) {
        subset_->textureUnits(units);
    }
    gl_.ActiveTexture(GL_TEXTURE1);
    gl_.ClientActiveTexture(GL_TEXTURE1);
    if (units > 1) {
        fxEnable(GL_TEXTURE_2D);
        gl_.EnableClientState(GL_TEXTURE_COORD_ARRAY);
        fxTexEnv(GL_MODULATE);
    } else {
        gl_.Disable(GL_TEXTURE_2D);
        gl_.DisableClientState(GL_TEXTURE_COORD_ARRAY);
    }
    gl_.ActiveTexture(GL_TEXTURE0);
    gl_.ClientActiveTexture(GL_TEXTURE0);
    textureUnits_ = units;
}

// What the neighborhood is drawn with: its vertices are in the world's space already.
void GlRenderer::fixedWorldMatrices(const FrameView& f) {
    gl_.ActiveTexture(GL_TEXTURE0);
    fxEnable(GL_TEXTURE_2D);
    fxTexEnv(GL_MODULATE);
    gl_.MatrixMode(GL_PROJECTION);
    gl_.LoadMatrixf(byColumns(clipSpace() * f.viewProjection).data());
    gl_.MatrixMode(GL_MODELVIEW);
    gl_.LoadIdentity();
}

// A lot's sky: a cube around the camera showing the cube map. Its corners are the directions the
// cube map is looked up by, which is what the shader works out per pixel from the camera's rays.
void GlRenderer::drawSkyFixed(const FrameView& f, const Texture& cube) {
    if (cube.id == 0 || skyVertices_ == 0) {
        return;
    }
    State state;
    state.depthTest = false;
    state.depthWrite = false;
    setState(state);
    fixedWorldMatrices(f);
    const float size = std::clamp(20 * f.nearZ, 1.0f, 100.0f);
    const Vec3& eye = f.camera->eye;
    Mat4 place;
    place.m = {size, 0, 0, eye[0], 0, size, 0, eye[1], 0, 0, size, eye[2], 0, 0, 0, 1};
    gl_.LoadMatrixf(byColumns(place).data());
    gl_.Disable(GL_TEXTURE_2D);
    fxEnable(GL_TEXTURE_CUBE_MAP);
    bindTexture(0, const_cast<Texture&>(cube), GL_CLAMP_TO_EDGE);
    gl_.Color4f(1, 1, 1, 1);
    gl_.BindBuffer(GL_ARRAY_BUFFER, skyVertices_);
    arrayBuffer_ = skyVertices_;
    setClientArrays(kVertexArray | kTexCoordArray);
    fxVertexPointer(3, GL_FLOAT, 0, nullptr);
    fxTexCoordPointer(3, GL_FLOAT, 0, nullptr);
    gl_.DrawArrays(GL_TRIANGLES, 0, 36);
    gl_.Disable(GL_TEXTURE_CUBE_MAP);
    fxEnable(GL_TEXTURE_2D);
}

// A lot skirt's boundary curtain, into depth only.
void GlRenderer::drawBoundaryFixed(const FrameView& f) {
    State state;
    state.cull = true;
    state.depthFunc = GL_GREATER;
    state.colorWrite = false;
    setState(state);
    fixedWorldMatrices(f);
    gl_.Disable(GL_TEXTURE_2D);
    gl_.BindBuffer(GL_ARRAY_BUFFER, gpu_->curtainVertices);
    gl_.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, gpu_->curtainIndices);
    arrayBuffer_ = gpu_->curtainVertices;
    elementBuffer_ = gpu_->curtainIndices;
    setClientArrays(kVertexArray);
    fxVertexPointer(3, GL_FLOAT, 3 * sizeof(float), nullptr);
    fxDrawElements(GL_TRIANGLES, 24, GL_UNSIGNED_INT, nullptr);
    fxEnable(GL_TEXTURE_2D);
}

// The terrain: a pass for each paint texture a cell shows, the passes added up. Two ways to
// weight a paint, switched with setSingleTextureTerrain:
//  - Two texture units: the paint times its weight over the map (the second unit) and the gradient
//    (the vertices' colours), then the light map multiplied in over all of it. The shader leaves the
//    light map off what is under the water; the water over it hides most of the difference.
//  - One: the paint times its vertices' colours, which hold its weight, the gradient and the light
//    map. Those are worked out at the vertices, so they blend linearly over a tile where the
//    shader has them per pixel.
void GlRenderer::drawTerrainFixed(const FrameView& f) {
    SceneGpu& g = *gpu_;
    const bool single = singleTextureTerrain_;
    fixedWorldMatrices(f);
    constexpr GLsizei stride = kFixedTerrainFloats * sizeof(float);
    fixedTextureUnits(single ? 1 : 2);
    setClientArrays(kVertexArray | kColorArray | kTexCoordArray);
    gl_.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, g.terrainIndices);
    elementBuffer_ = g.terrainIndices;
    gl_.BindBuffer(GL_ARRAY_BUFFER, g.terrainVertices);
    fxVertexPointer(3, GL_FLOAT, stride, nullptr);
    fxTexCoordPointer(2, GL_FLOAT, stride, offset(3 * sizeof(float)));
    if (!single) {
        gl_.ClientActiveTexture(GL_TEXTURE1);
        fxTexCoordPointer(2, GL_FLOAT, stride, offset(5 * sizeof(float)));
        gl_.ClientActiveTexture(GL_TEXTURE0);
        gl_.BindBuffer(GL_ARRAY_BUFFER, g.terrainColors);
        fxColorPointer(4, GL_UNSIGNED_BYTE, 0, nullptr);
    }
    arrayBuffer_ = ~0u;  // not what the caches think it is now
    std::vector<std::uint8_t> slices = g.terrainSlices;
    if (slices.empty()) {
        slices.push_back(0);
    }
    bool first = true;
    for (std::size_t pass = 0; pass < slices.size(); ++pass) {
        const std::size_t slice = slices[pass];
        if (slice >= g.terrainPaints.size() || pass >= g.terrainWeights.size() || pass >= g.terrainSliceColors.size()) {
            continue;
        }
        // The first pass replaces what was drawn before, the others add to it.
        State state;
        state.blend = first ? 0 : 5;
        state.depthWrite = first;
        setState(state);
        first = false;
        if (single) {
            gl_.BindBuffer(GL_ARRAY_BUFFER, g.terrainSliceColors[pass]);
            fxColorPointer(4, GL_UNSIGNED_BYTE, 0, nullptr);
        }
        bindTexture(0, g.terrainPaints[slice], GL_REPEAT);
        if (!single) {
            bindTexture(1, g.terrainWeights[pass], GL_CLAMP_TO_EDGE);
        }
        fxDrawElements(GL_TRIANGLES, static_cast<GLsizei>(g.terrainIndexCount), GL_UNSIGNED_INT, nullptr);
    }
    fixedTextureUnits(1);
    if (first || single) {
        return;
    }
    State lit;
    lit.depthWrite = false;
    setState(lit);
    fxEnable(GL_BLEND);
    fxBlendFunc(GL_DST_COLOR, GL_ZERO, GL_ZERO, GL_ONE);
    state_.reset();
    setClientArrays(kVertexArray | kTexCoordArray);
    gl_.Color4f(1, 1, 1, 1);
    gl_.BindBuffer(GL_ARRAY_BUFFER, g.terrainVertices);
    fxTexCoordPointer(2, GL_FLOAT, stride, offset(7 * sizeof(float)));
    bindTexture(0, g.terrainLight, GL_REPEAT);
    fxDrawElements(GL_TRIANGLES, static_cast<GLsizei>(g.terrainIndexCount), GL_UNSIGNED_INT, nullptr);
}

// The roads: their texture blended over the terrain, times the terrain's light map at the same spot,
// which is the second texture unit, or with one texture the vertices' colours.
void GlRenderer::drawRoadsFixed(const FrameView& f) {
    SceneGpu& g = *gpu_;
    const bool single = singleTextureTerrain_ && g.roadColors != 0;
    fixedWorldMatrices(f);
    State state;
    state.blend = 1;
    setState(state);
    constexpr GLsizei stride = kFixedRoadFloats * sizeof(float);
    fixedTextureUnits(single ? 1 : 2);
    setClientArrays(kVertexArray | kTexCoordArray | (single ? kColorArray : 0u));
    gl_.Color4f(1, 1, 1, 1);
    gl_.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, g.roadIndices);
    elementBuffer_ = g.roadIndices;
    gl_.BindBuffer(GL_ARRAY_BUFFER, g.roadVertices);
    fxVertexPointer(3, GL_FLOAT, stride, nullptr);
    fxTexCoordPointer(2, GL_FLOAT, stride, offset(3 * sizeof(float)));
    if (single) {
        gl_.BindBuffer(GL_ARRAY_BUFFER, g.roadColors);
        fxColorPointer(4, GL_UNSIGNED_BYTE, 0, nullptr);
    } else {
        gl_.ClientActiveTexture(GL_TEXTURE1);
        fxTexCoordPointer(2, GL_FLOAT, stride, offset(5 * sizeof(float)));
        gl_.ClientActiveTexture(GL_TEXTURE0);
        bindTexture(1, g.terrainLight, GL_REPEAT);
    }
    arrayBuffer_ = ~0u;
    for (const auto& run : g.roadRuns) {
        if (run.texture >= g.roadTextures.size()) {
            continue;
        }
        bindTexture(0, g.roadTextures[run.texture], GL_CLAMP_TO_EDGE);
        fxDrawElements(GL_TRIANGLES, static_cast<GLsizei>(run.indexCount), GL_UNSIGNED_INT, offset(static_cast<std::size_t>(run.firstIndex) * 4));
    }
    fixedTextureUnits(1);
}

// The lots' imposters, as drawImposters has them: terrain (its light map multiplied over what is
// there, then its texture over that), then walls, slices and roofs.
void GlRenderer::drawImpostersFixed(const FrameView& f) {
    SceneGpu& g = *gpu_;
    using Kind = scene::PaintedTerrain::ImposterPart::Kind;
    fixedWorldMatrices(f);
    gl_.BindBuffer(GL_ARRAY_BUFFER, g.imposterVertices);
    gl_.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, g.imposterIndices);
    arrayBuffer_ = g.imposterVertices;
    elementBuffer_ = g.imposterIndices;
    setClientArrays(kVertexArray | kTexCoordArray);
    constexpr GLsizei stride = kImposterFloats * sizeof(float);
    // `secondSet` takes the texture coordinates of the second set, `color` multiplies the texture, and
    // pixels with alpha at or below `cutoff` are dropped (none for a cutoff below 0).
    const auto draw = [&](const State& state, const ImposterMesh::Part& d, int texture, GLint wrap, bool nearest, float cutoff, bool secondSet,
                          const std::array<float, 3>& color) {
        setState(state);
        gl_.Color4f(color[0], color[1], color[2], 1);
        if (cutoff >= 0) {
            fxEnable(GL_ALPHA_TEST);
            fxAlphaFunc(GL_GREATER, cutoff);
        } else {
            gl_.Disable(GL_ALPHA_TEST);
        }
        bindTexture(0, g.imposterTextures[static_cast<std::size_t>(texture)], wrap, nearest);
        const std::size_t start = static_cast<std::size_t>(d.vertexOffset) * stride;
        fxVertexPointer(3, GL_FLOAT, stride, offset(start));
        fxTexCoordPointer(2, GL_FLOAT, stride, offset(start + (secondSet ? 5 : 3) * sizeof(float)));
        fxDrawElements(GL_TRIANGLES, static_cast<GLsizei>(d.indexCount), GL_UNSIGNED_INT, offset(static_cast<std::size_t>(d.firstIndex) * 4));
    };
    State light, terrain, wall, roof;
    light.blend = 3;
    light.cull = true;
    light.depthWrite = false;
    terrain.blend = 4;
    terrain.cull = true;
    terrain.depthWrite = false;
    wall.blend = 4;
    wall.cull = true;
    const std::array<float, 3> lit{imposterLight_[0], imposterLight_[1], imposterLight_[2]};
    for (const auto& d : g.imposterDraws) {
        if (d.kind != Kind::Terrain) {
            continue;
        }
        if (d.lightMap >= 0) {
            draw(light, d, d.lightMap, GL_REPEAT, false, 0, true, {d.tint[0], d.tint[1], d.tint[2]});
        }
        draw(terrain, d, static_cast<int>(d.texture), GL_CLAMP_TO_EDGE, false, 0, false, lit);
    }
    for (const auto& d : g.imposterDraws) {
        switch (d.kind) {
        case Kind::Wall:
            draw(wall, d, static_cast<int>(d.texture), GL_REPEAT, true, 64.0f / 255, false, lit);
            break;
        case Kind::Slice:
            draw(wall, d, static_cast<int>(d.texture), GL_REPEAT, true, 127.0f / 255, false, lit);
            break;
        case Kind::Roof:
            draw(roof, d, static_cast<int>(d.texture), GL_CLAMP_TO_EDGE, false, -1, false, lit);
            break;
        case Kind::Terrain:
            break;
        }
    }
    gl_.Disable(GL_ALPHA_TEST);
}

// The water: a sheet of the sky's colour, tinted, as see-through as the reflection would be strong.
// With no framebuffer to draw a reflection into, this is all the water shows.
void GlRenderer::drawWaterFixed(const FrameView& f) {
    SceneGpu& g = *gpu_;
    fixedWorldMatrices(f);
    State state;
    state.blend = 1;
    state.depthWrite = false;
    setState(state);
    gl_.Disable(GL_TEXTURE_2D);
    gl_.Color4f(g.waterFlat[0], g.waterFlat[1], g.waterFlat[2], g.waterFlat[3]);
    gl_.BindBuffer(GL_ARRAY_BUFFER, g.waterVertices);
    gl_.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, g.waterIndices);
    arrayBuffer_ = g.waterVertices;
    elementBuffer_ = g.waterIndices;
    setClientArrays(kVertexArray);
    fxVertexPointer(3, GL_FLOAT, kWaterFloats * sizeof(float), nullptr);
    fxDrawElements(GL_TRIANGLES, static_cast<GLsizei>(g.waterIndexCount), GL_UNSIGNED_INT, nullptr);
    fxEnable(GL_TEXTURE_2D);
}

} // namespace ts2::render
