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

constexpr unsigned kVertexArray = 1, kColorArray = 2, kTexCoordArray = 4;
// Where a vertex keeps what (see kVertexFloats).
constexpr std::size_t kNormalAt = 3, kAlphaAt = 8, kLightUvAt = 9, kColorAt = 11;
// The light a surface that a light map would darken is drawn with where it can't be multiplied in
// (see drawListFixed), as a share of the light map's gain.
constexpr float kBlendedLightMapShare = 0.5f;

const void* offset(std::size_t bytes) {
    return reinterpret_cast<const void*>(bytes);
}

// Where a vertex's attribute starts in a buffer of blocks (see uploadBlocks): `before` floats of each of the
// `count` vertices come ahead of the attribute's block, which has `size` floats for each.
const void* inBlocks(std::size_t count, std::size_t before, std::size_t size, std::size_t first) {
    return offset((count * before + first * size) * sizeof(float));
}

GLenum indexType(bool wide) {
    return wide ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT;
}

std::size_t indexBytes(bool wide) {
    return wide ? 4 : 2;
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
// when the light map itself can't be multiplied in. `scale` is what the colours are multiplied by on their
// way to bytes: 0.5 where the texture environment multiplies by 2 again (see fxTexEnvScale).
void vertexColors(const VertexUniforms& vu, const FragmentUniforms& fu, const float* vertices, unsigned count, float lightMapShare, float scale,
                  const scene::Image* lightImage, const scene::Image* lightFrom, std::vector<std::uint8_t>& out) {
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
            // Floors and walls: the texture alone or times the material's colour, then the light map. Taken from
            // the light map's pixels at the vertex where it has them, else `lightMapShare` of its gain.
            std::array<float, 3> lit{1, 1, 1};
            if (lightImage != nullptr) {
                lit = sampleLightMap(*lightImage, v[kLightUvAt], v[kLightUvAt + 1]);
                if (lightFrom != nullptr && fu.lightMapFade < 1) {
                    const auto from = sampleLightMap(*lightFrom, v[kLightUvAt], v[kLightUvAt + 1]);
                    for (std::size_t k = 0; k < 3; ++k) {
                        lit[k] = from[k] + (lit[k] - from[k]) * fu.lightMapFade;
                    }
                }
            }
            for (std::size_t k = 0; k < 3; ++k) {
                float factor = lightMapShare;
                if (lightImage != nullptr) {
                    factor = lit[k] * fu.lightMap[0];
                    if (fu.lightMap[2] > 0.5f) {  // the lot's terrain: darkened by its light map, never brightened
                        factor = std::min(1.0f, factor);
                    }
                }
                rgb[k] = (fu.lightMap[1] > 0.5f && hasTexture ? 1.0f : color[k] * vc[k]) * factor;
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
        at[0] = byteOf(rgb[0] * scale);
        at[1] = byteOf(rgb[1] * scale);
        at[2] = byteOf(rgb[2] * scale);
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
    if (srcRgb == srcAlpha && dstRgb == dstAlpha) {
        gl_.BlendFunc(srcRgb, dstRgb);
    } else {
        gl_.BlendFuncSeparate(srcRgb, dstRgb, srcAlpha, dstAlpha);
    }
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

// Multiplies the texture times the vertex colour by `scale` (1 or 2). Twice is how colours brighter than 1 get
// past the byte they are kept in: the GS does it by calling 0x80 full brightness, and OpenGL by combining.
void GlRenderer::fxTexEnvScale(int scale) {
    if (scale > 1) {
        if (subset_) {
            subset_->overbright();
        }
        gl_.TexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
        gl_.TexEnvi(GL_TEXTURE_ENV, GL_RGB_SCALE, scale);
        texEnvScaled_ = true;
    } else {
        fxTexEnv(GL_MODULATE);
        if (texEnvScaled_) {
            gl_.TexEnvi(GL_TEXTURE_ENV, GL_RGB_SCALE, 1);
            texEnvScaled_ = false;
        }
    }
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

// How many pixels one repeat of the texture covers across and down in this draw, on a console's screen: for a few of
// its triangles, how the screen position changes with the texture coordinates, and the longer of u and v. The most
// any draw asked for is what the image needs to be as big as.
std::optional<std::array<float, 2>> GlRenderer::noteDensity(const GpuPrimitive& p, const MeshBuffers& buffers, const Mat4& clip, int image, float scale,
                                                            float cap) {
    if (buffers.cpuIndices.empty() || buffers.cpuVertices.empty() || viewWidth_ == 0) {
        return std::nullopt;
    }
    const std::size_t triangles = p.count / 3;
    const std::size_t samples = std::min<std::size_t>(triangles, 12);
    const float scaleX = static_cast<float>(consoleWidth_), scaleY = static_cast<float>(consoleWidth_) * static_cast<float>(viewHeight_) / static_cast<float>(viewWidth_);
    double sumU = 0, sumV = 0;
    std::size_t used = 0;
    for (std::size_t s = 0; s < samples; ++s) {
        const std::size_t t = s * triangles / samples;
        float px[3][2], uv[3][2];
        bool ok = true;
        for (std::size_t k = 0; k < 3 && ok; ++k) {
            const std::size_t at = static_cast<std::size_t>(p.firstIndex) + t * 3 + k;
            if (at >= buffers.cpuIndices.size()) {
                ok = false;
                break;
            }
            const std::size_t vertex = static_cast<std::size_t>(p.firstVertex) + buffers.cpuIndices[at];
            if ((vertex + 1) * kVertexFloats > buffers.cpuVertices.size()) {
                ok = false;
                break;
            }
            const float* v = &buffers.cpuVertices[vertex * kVertexFloats];
            const auto& m = clip.m;
            const float cw = m[12] * v[0] + m[13] * v[1] + m[14] * v[2] + m[15];
            if (cw <= 1e-4f) {
                ok = false;
                break;
            }
            px[k][0] = ((m[0] * v[0] + m[1] * v[1] + m[2] * v[2] + m[3]) / cw * 0.5f + 0.5f) * scaleX;
            px[k][1] = ((m[4] * v[0] + m[5] * v[1] + m[6] * v[2] + m[7]) / cw * 0.5f + 0.5f) * scaleY;
            uv[k][0] = v[6];
            uv[k][1] = v[7];
        }
        if (!ok) {
            continue;
        }
        const float e1u = uv[1][0] - uv[0][0], e1v = uv[1][1] - uv[0][1], e2u = uv[2][0] - uv[0][0], e2v = uv[2][1] - uv[0][1];
        const float det = e1u * e2v - e2u * e1v;
        if (std::fabs(det) < 1e-9f) {
            continue;
        }
        const float q1x = px[1][0] - px[0][0], q1y = px[1][1] - px[0][1], q2x = px[2][0] - px[0][0], q2y = px[2][1] - px[0][1];
        const float pux = (q1x * e2v - q2x * e1v) / det, puy = (q1y * e2v - q2y * e1v) / det;
        const float pvx = (q2x * e1u - q1x * e2u) / det, pvy = (q2y * e1u - q1y * e2u) / det;
        sumU += std::hypot(pux, puy);
        sumV += std::hypot(pvx, pvy);
        ++used;
    }
    if (used == 0) {
        return std::nullopt;
    }
    const std::array<float, 2> here{std::min(cap, scale * static_cast<float>(sumU / static_cast<double>(used))),
                                    std::min(cap, scale * static_cast<float>(sumV / static_cast<double>(used)))};
    if (subset_) {
        auto& need = gpu_->neededPixels[image];
        need[0] = std::max(need[0], here[0]);
        need[1] = std::max(need[1], here[1]);
    }
    return here;
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
// normal-mapped light, specular and environment cubes. A floor's or wall's light map is sampled at its
// vertices (one sample a tile on a floor, where the map has several). With the multiplying pass instead,
// blended light-mapped surfaces (floor decals) can't have the light map multiplied in over what is behind
// them, so they are darkened by a share of its gain.
void GlRenderer::drawListFixed(const FrameView& f, const std::vector<Draw>& list, int blend) {
    if (list.empty()) {
        return;
    }
    gl_.ActiveTexture(GL_TEXTURE0);
    fxEnable(GL_TEXTURE_2D);
    const float colorScale = overbright_ ? 0.5f : 1.0f;
    fxTexEnvScale(overbright_ ? 2 : 1);
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
        if (subset_) {
            for (const SamplerBinding& binding : {u.samplers[0], u.samplers[1]}) {
                if (binding.texture.kind == TextureRef::Kind::Image) {
                    gpu_->drawnImages.insert(binding.texture.index);
                }
            }
        }
        if ((subset_ || detail_ > 0) && u.samplers[0].texture.kind == TextureRef::Kind::Image) {
            const int image = u.samplers[0].texture.index;
            // How sharp it needs to be depends on what it is: floors, walls and the like may be soft, however close the
            // camera, and a Sim is looked at more than a thing, so gets twice the texels.
            const scene::Part part = d.instance->part;
            const bool surface = part == scene::Part::Floor || part == scene::Part::Wall || part == scene::Part::WallHub || part == scene::Part::Skirt ||
                                 part == scene::Part::Roof;
            const bool sim = m && m->simLighting;
            const float scale = (detail_ > 0 ? detail_ : 1.0f) * (sim ? 2.0f : 1.0f);
            const float cap = surface ? surfaceSide_ : sim ? simSide_ : objectSide_;
            if (const auto need = noteDensity(p, buffers, f.viewProjection * fromRows(vu.model), image, scale, cap); need && detail_ > 0) {
                growTexture(image, *need);
            }
        }

        // The vertices as they are in the model's space: a skinned primitive's are posed (into the streaming buffer).
        const float* vertices = nullptr;
        if (p.skinned) {
            poseSkinned(p, buffers, static_cast<std::size_t>(d.instance - instances_.data()));
            vertices = scratch_.data();
        } else {
            vertices = &buffers.cpuVertices[static_cast<std::size_t>(p.firstVertex) * kVertexFloats];
        }
        // A floor's or wall's light is taken from its light map at the vertices where the pixels are kept. If not (or when
        // setVertexLightMaps asks for it), it is multiplied in by a pass of its own over what the first drew.
        const bool lightMapped = fu.lightMap[0] > 0;
        const scene::Image* lightImage = nullptr;
        const scene::Image* lightFrom = nullptr;
        if (lightMapped && vertexLightMaps_) {
            if (const auto found = gpu_->lightImages.find(d.instance->lightMap); found != gpu_->lightImages.end()) {
                lightImage = &found->second;
                if (const auto from = gpu_->lightFrom.find(d.instance->lightMap); from != gpu_->lightFrom.end()) {
                    lightFrom = &from->second;
                }
            }
        }
        const bool multiplied = lightMapped && blend == 0 && lightImage == nullptr;
        vertexColors(vu, fu, vertices, p.vertices,
                     lightMapped && !multiplied && lightImage == nullptr ? std::min(1.0f, kBlendedLightMapShare * fu.lightMap[0]) : 1.0f, colorScale,
                     lightImage, lightFrom, litColors_);

        // The lit colours, then the vertices they belong to.
        const auto colorBytes = static_cast<GLsizeiptr>(litColors_.size());
        gl_.BindBuffer(GL_ARRAY_BUFFER, colorStream_);
        gl_.BufferData(GL_ARRAY_BUFFER, colorBytes, nullptr, GL_STREAM_DRAW);
        gl_.BufferData(GL_ARRAY_BUFFER, colorBytes, litColors_.data(), GL_STREAM_DRAW);
        setClientArrays(kVertexArray | kColorArray | kTexCoordArray);
        fxColorPointer(4, GL_UNSIGNED_BYTE, 0, nullptr);
        arrayBuffer_ = colorStream_;
        // The positions: a posed primitive's are alone in the streaming buffer, the others are in the mesh's. The
        // texture coordinates and the light map's are always in the mesh's.
        const std::size_t count = buffers.vertexCount, first = p.firstVertex;
        gl_.BindBuffer(GL_ARRAY_BUFFER, p.skinned ? streamVertices_ : buffers.vertices);
        fxVertexPointer(3, GL_FLOAT, 0, p.skinned ? offset(0) : inBlocks(count, 0, 3, first));
        gl_.BindBuffer(GL_ARRAY_BUFFER, buffers.vertices);
        arrayBuffer_ = buffers.vertices;
        fxTexCoordPointer(2, GL_FLOAT, 0, inBlocks(count, 3, 2, first));
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
        const GLenum kind = indexType(buffers.wideIndices);
        const auto indices = offset(static_cast<std::size_t>(p.firstIndex) * indexBytes(buffers.wideIndices));
        fxDrawElements(GL_TRIANGLES, static_cast<GLsizei>(p.count), kind, indices);

        if (multiplied) {
            // The light map times what the first pass drew (the gain is the pass's colour), writing no depth.
            gl_.Disable(GL_ALPHA_TEST);
            fxEnable(GL_BLEND);
            fxBlendFunc(GL_DST_COLOR, GL_ZERO, GL_DST_COLOR, GL_ZERO);
            gl_.DepthMask(GL_FALSE);
            setClientArrays(kVertexArray | kTexCoordArray);
            const float gain = fu.lightMap[0] * colorScale;
            gl_.Color4f(gain, gain, gain, 1);
            fxTexCoordPointer(2, GL_FLOAT, 0, inBlocks(count, 5, 2, first));
            bindTexture(0, textureFor(u.samplers[1].texture), GL_CLAMP_TO_EDGE);
            fxDrawElements(GL_TRIANGLES, static_cast<GLsizei>(p.count), kind, indices);
            state_.reset();  // the blend and depth mask are not what setState thinks they are
        }
    }
    fxTexEnvScale(1);
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

// Points the arrays at the sprites' vertices, bound now: the positions, then the texture coordinates, then
// the colours, each a block of tightly packed values.
void GlRenderer::fixedSpritePointers(std::size_t vertices) {
    setClientArrays(kVertexArray | kColorArray | kTexCoordArray);
    fxVertexPointer(2, GL_FLOAT, 0, inBlocks(vertices, 0, 2, 0));
    fxTexCoordPointer(2, GL_FLOAT, 0, inBlocks(vertices, 2, 2, 0));
    fxColorPointer(4, GL_FLOAT, 0, inBlocks(vertices, 4, 4, 0));
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

// A lot's sky: a box around the camera with a picture on each side. Its corners are the directions the sky
// is looked up by, which is what the shader works out per pixel from the camera's rays.
void GlRenderer::drawSkyFixed(const FrameView& f) {
    SceneGpu& g = *gpu_;
    if (g.skyFaces[0].id == 0 || skyVertices_ == 0) {
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
    gl_.Color4f(1, 1, 1, 1);
    gl_.BindBuffer(GL_ARRAY_BUFFER, skyVertices_);
    arrayBuffer_ = skyVertices_;
    setClientArrays(kVertexArray | kTexCoordArray);
    fxVertexPointer(3, GL_FLOAT, 0, nullptr);
    fxTexCoordPointer(2, GL_FLOAT, 0, offset(36 * 3 * sizeof(float)));
    for (int face = 0; face < 6; ++face) {
        bindTexture(0, g.skyFaces[static_cast<std::size_t>(face)], GL_CLAMP_TO_EDGE);
        gl_.DrawArrays(GL_TRIANGLES, face * 6, 6);
    }
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
    fxVertexPointer(3, GL_FLOAT, 0, nullptr);
    fxDrawElements(GL_TRIANGLES, 24, indexType(gpu_->curtainWide), nullptr);
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
    const std::size_t count = g.terrainVertexCount;
    const GLenum kind = indexType(g.terrainWide);
    fixedTextureUnits(single ? 1 : 2);
    setClientArrays(kVertexArray | kColorArray | kTexCoordArray);
    gl_.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, g.terrainIndices);
    elementBuffer_ = g.terrainIndices;
    gl_.BindBuffer(GL_ARRAY_BUFFER, g.terrainVertices);
    fxVertexPointer(3, GL_FLOAT, 0, inBlocks(count, 0, 3, 0));
    fxTexCoordPointer(2, GL_FLOAT, 0, inBlocks(count, 3, 2, 0));
    if (!single) {
        gl_.ClientActiveTexture(GL_TEXTURE1);
        fxTexCoordPointer(2, GL_FLOAT, 0, inBlocks(count, 5, 2, 0));
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
        fxDrawElements(GL_TRIANGLES, static_cast<GLsizei>(g.terrainIndexCount), kind, nullptr);
    }
    fixedTextureUnits(1);
    if (first || single) {
        return;
    }
    State lit;
    lit.depthWrite = false;
    setState(lit);
    fxEnable(GL_BLEND);
    fxBlendFunc(GL_DST_COLOR, GL_ZERO, GL_DST_COLOR, GL_ZERO);
    state_.reset();
    setClientArrays(kVertexArray | kTexCoordArray);
    gl_.Color4f(1, 1, 1, 1);
    gl_.BindBuffer(GL_ARRAY_BUFFER, g.terrainVertices);
    fxTexCoordPointer(2, GL_FLOAT, 0, inBlocks(count, 7, 2, 0));
    bindTexture(0, g.terrainLight, GL_REPEAT);
    fxDrawElements(GL_TRIANGLES, static_cast<GLsizei>(g.terrainIndexCount), kind, nullptr);
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
    const std::size_t count = g.roadVertexCount;
    const GLenum kind = indexType(g.roadWide);
    const std::size_t indexSize = indexBytes(g.roadWide);
    fixedTextureUnits(single ? 1 : 2);
    setClientArrays(kVertexArray | kTexCoordArray | (single ? kColorArray : 0u));
    gl_.Color4f(1, 1, 1, 1);
    gl_.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, g.roadIndices);
    elementBuffer_ = g.roadIndices;
    gl_.BindBuffer(GL_ARRAY_BUFFER, g.roadVertices);
    fxVertexPointer(3, GL_FLOAT, 0, inBlocks(count, 0, 3, 0));
    fxTexCoordPointer(2, GL_FLOAT, 0, inBlocks(count, 3, 2, 0));
    if (single) {
        gl_.BindBuffer(GL_ARRAY_BUFFER, g.roadColors);
        fxColorPointer(4, GL_UNSIGNED_BYTE, 0, nullptr);
    } else {
        gl_.ClientActiveTexture(GL_TEXTURE1);
        fxTexCoordPointer(2, GL_FLOAT, 0, inBlocks(count, 5, 2, 0));
        gl_.ClientActiveTexture(GL_TEXTURE0);
        bindTexture(1, g.terrainLight, GL_REPEAT);
    }
    arrayBuffer_ = ~0u;
    for (const auto& run : g.roadRuns) {
        if (run.texture >= g.roadTextures.size()) {
            continue;
        }
        bindTexture(0, g.roadTextures[run.texture], GL_CLAMP_TO_EDGE);
        fxDrawElements(GL_TRIANGLES, static_cast<GLsizei>(run.indexCount), kind, offset(static_cast<std::size_t>(run.firstIndex) * indexSize));
    }
    fixedTextureUnits(1);
}

// The lots' imposters, as drawImposters has them: terrain, then walls, slices and roofs.
void GlRenderer::drawImpostersFixed(const FrameView& f) {
    SceneGpu& g = *gpu_;
    using Kind = scene::PaintedTerrain::ImposterPart::Kind;
    fixedWorldMatrices(f);
    gl_.BindBuffer(GL_ARRAY_BUFFER, g.imposterVertices);
    gl_.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, g.imposterIndices);
    arrayBuffer_ = g.imposterVertices;
    elementBuffer_ = g.imposterIndices;
    setClientArrays(kVertexArray | kTexCoordArray);
    const std::size_t count = g.imposterVertexCount;
    const GLenum kind = indexType(g.imposterWide);
    const std::size_t indexSize = indexBytes(g.imposterWide);
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
        const auto first = static_cast<std::size_t>(d.vertexOffset);
        fxVertexPointer(3, GL_FLOAT, 0, inBlocks(count, 0, 3, first));
        fxTexCoordPointer(2, GL_FLOAT, 0, inBlocks(count, secondSet ? 5 : 3, 2, first));
        fxDrawElements(GL_TRIANGLES, static_cast<GLsizei>(d.indexCount), kind, offset(static_cast<std::size_t>(d.firstIndex) * indexSize));
    };
    // Their textures are not premultiplied here (see uploadTerrain), so they blend over by their alpha. The terrain's
    // light map multiplied over what is there first is left out: it only shows through the ground texture's
    // see-through edges, and the consoles can't multiply a colour by a colour.
    State terrain, wall, roof;
    terrain.blend = 1;
    terrain.cull = true;
    terrain.depthWrite = false;
    wall.blend = 1;
    wall.cull = true;
    const std::array<float, 3> lit{imposterLight_[0], imposterLight_[1], imposterLight_[2]};
    for (const auto& d : g.imposterDraws) {
        if (d.kind == Kind::Terrain) {
            draw(terrain, d, static_cast<int>(d.texture), GL_CLAMP_TO_EDGE, false, 0, false, lit);
        }
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
    fxVertexPointer(3, GL_FLOAT, 0, nullptr);
    fxDrawElements(GL_TRIANGLES, static_cast<GLsizei>(g.waterIndexCount), indexType(g.waterWide), nullptr);
    fxEnable(GL_TEXTURE_2D);
}

} // namespace ts2::render
