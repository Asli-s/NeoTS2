#include "diag/timing.hpp"
#include "render/gl/gl_renderer.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ts2::render {

namespace {

constexpr GLsizei kVertexBytes = static_cast<GLsizei>(kVertexFloats * sizeof(float));
constexpr unsigned kSceneAttributes = (1u << 0) | (1u << 1) | (1u << 2) | (1u << 3) | (1u << 6) | (1u << 7) | (1u << 8);
constexpr unsigned kSpriteAttributes = (1u << 0) | (1u << 1) | (1u << 2);
constexpr unsigned kSpriteFloats = 8;  // position, texture coordinates, color

const void* offset(std::size_t bytes) {
    return reinterpret_cast<const void*>(bytes);
}

} // namespace

void GlRenderer::invalidateState() {
    program_ = ~0u;
    attributeMask_ = ~0u;
    state_.reset();
    arrayBuffer_ = elementBuffer_ = ~0u;
    bound_ = {};
}

void GlRenderer::useProgram(const Program& program) {
    if (program_ != program.id) {
        gl_.UseProgram(program.id);
        program_ = program.id;
    }
}

void GlRenderer::setAttributes(unsigned mask) {
    const bool unknown = attributeMask_ == ~0u;
    for (unsigned i = 0; i < 12; ++i) {
        const bool want = (mask >> i) & 1u, have = !unknown && ((attributeMask_ >> i) & 1u);
        if (want && (unknown || !have)) {
            gl_.EnableVertexAttribArray(i);
        } else if (!want && (unknown || have)) {
            gl_.DisableVertexAttribArray(i);
        }
    }
    attributeMask_ = mask;
}

// Sets what a draw needs of the GL state that isn't what the last draw left, as the SDL GPU
// renderer's pipelines have it.
void GlRenderer::setState(const State& want) {
    const State& have = state_.value_or(State{});
    const bool unknown = !state_.has_value();
    if (unknown || have.blend != want.blend) {
        switch (want.blend) {
        case 0:
            gl_.Disable(GL_BLEND);
            break;
        case 1:  // over
            gl_.Enable(GL_BLEND);
            gl_.BlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            break;
        case 2:  // added
            gl_.Enable(GL_BLEND);
            gl_.BlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ZERO, GL_ONE);
            break;
        case 3:  // multiplied into what is there
            gl_.Enable(GL_BLEND);
            gl_.BlendFuncSeparate(GL_DST_COLOR, GL_ONE_MINUS_SRC_ALPHA, GL_DST_COLOR, GL_ONE_MINUS_SRC_ALPHA);
            break;
        case 4:  // over, the colour already multiplied by its alpha
            gl_.Enable(GL_BLEND);
            gl_.BlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            break;
        default:  // summed
            gl_.Enable(GL_BLEND);
            gl_.BlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_ONE);
            break;
        }
    }
    if (unknown || have.cull != want.cull) {
        if (want.cull) {
            gl_.Enable(GL_CULL_FACE);
        } else {
            gl_.Disable(GL_CULL_FACE);
        }
    }
    if (unknown || have.depthTest != want.depthTest) {
        if (want.depthTest) {
            gl_.Enable(GL_DEPTH_TEST);
        } else {
            gl_.Disable(GL_DEPTH_TEST);
        }
    }
    if (unknown || have.depthWrite != want.depthWrite) {
        gl_.DepthMask(want.depthWrite ? GL_TRUE : GL_FALSE);
    }
    if (unknown || have.depthFunc != want.depthFunc) {
        gl_.DepthFunc(want.depthFunc);
    }
    if (unknown || have.colorWrite != want.colorWrite) {
        const GLboolean on = want.colorWrite ? GL_TRUE : GL_FALSE;
        gl_.ColorMask(on, on, on, on);
    }
    state_ = want;
}

// Points an attribute at its place in the bound vertex buffer.
void GlRenderer::attributes(unsigned location, int size, GLsizei stride, std::size_t at) {
    gl_.VertexAttribPointer(location, size, GL_FLOAT, GL_FALSE, stride, offset(at));
}

void GlRenderer::bindTexture(unsigned unit, Texture& texture, GLint wrap, bool nearest) {
    const GLint how = wrap + (nearest ? 0x10000 : 0);
    if (bound_[unit].first == texture.id && bound_[unit].second == how) {
        return;
    }
    gl_.ActiveTexture(GL_TEXTURE0 + unit);
    gl_.BindTexture(texture.target, texture.id);
    if (texture.target == GL_TEXTURE_2D) {
        if (texture.wrap != wrap) {
            gl_.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
            gl_.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
            texture.wrap = wrap;
        }
        if (texture.nearest != nearest) {
            gl_.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, nearest ? GL_NEAREST : texture.mips ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
            gl_.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, nearest ? GL_NEAREST : GL_LINEAR);
            texture.nearest = nearest;
        }
    }
    bound_[unit] = {texture.id, how};
    // Another unit with this texture could now see another way of sampling it than it was told of.
    for (unsigned u = 0; u < bound_.size(); ++u) {
        if (u != unit && bound_[u].first == texture.id) {
            bound_[u].second = -1;
        }
    }
}

GlRenderer::Texture& GlRenderer::textureFor(const TextureRef& ref) {
    const auto at = static_cast<std::size_t>(ref.index);
    switch (ref.kind) {
    case TextureRef::Kind::Image:
        return gpu_->textures[at];
    case TextureRef::Kind::BlackCube:
        return gpu_->blackCube;
    case TextureRef::Kind::EnvCube:
        return gpu_->envCubes[at];
    case TextureRef::Kind::FadeFrom:
        return gpu_->fadingFrom.at(ref.index);
    case TextureRef::Kind::White:
        break;
    }
    return gpu_->white;
}

// The vertices of a skinned primitive as its joints pose them, in the model's space: each follows up
// to four joints, by their weights (see skinned.vert.hlsl, which does the same on the GPU).
void GlRenderer::poseSkinned(const GpuPrimitive& p, const MeshBuffers& buffers, std::size_t instance) {
    scratch_.resize(static_cast<std::size_t>(p.vertices) * kVertexFloats);
    const float* source = &buffers.cpuVertices[static_cast<std::size_t>(p.firstVertex) * kVertexFloats];
    const std::uint8_t* skin = &buffers.cpuSkin[p.firstSkin];
    const float* joints = jointData_.data();
    const auto base = static_cast<std::size_t>(jointBase_[instance]);
    for (std::size_t v = 0; v < p.vertices; ++v) {
        const float* in = source + v * kVertexFloats;
        float* out = &scratch_[v * kVertexFloats];
        std::copy_n(in, kVertexFloats, out);
        const std::uint8_t* ids = skin + v * kSkinBytes;
        float weights[4];
        std::memcpy(weights, ids + 4, sizeof(weights));
        float position[3] = {}, normal[3] = {}, tangent[3] = {};
        for (int k = 0; k < 4; ++k) {
            if (ids[k] == 255 || !(weights[k] > 0)) {
                continue;
            }
            const float* m = joints + (base + ids[k]) * 12;
            for (int r = 0; r < 3; ++r) {
                const float* row = m + r * 4;
                position[r] += weights[k] * (row[0] * in[0] + row[1] * in[1] + row[2] * in[2] + row[3]);
                normal[r] += weights[k] * (row[0] * in[3] + row[1] * in[4] + row[2] * in[5]);
                tangent[r] += weights[k] * (row[0] * in[14] + row[1] * in[15] + row[2] * in[16]);
            }
        }
        const float length = std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
        for (int r = 0; r < 3; ++r) {
            out[r] = position[r];
            out[3 + r] = length > 0 ? normal[r] / length : normal[r];
            out[14 + r] = tangent[r];
        }
    }
    const auto bytes = static_cast<GLsizeiptr>(scratch_.size() * sizeof(float));
    gl_.BindBuffer(GL_ARRAY_BUFFER, streamVertices_);
    arrayBuffer_ = streamVertices_;
    // The buffer is let go of first, so the driver needn't wait for the draws still reading the last one.
    gl_.BufferData(GL_ARRAY_BUFFER, bytes, nullptr, GL_STREAM_DRAW);
    gl_.BufferData(GL_ARRAY_BUFFER, bytes, scratch_.data(), GL_STREAM_DRAW);
}

// Draws primitives with the scene program (`blend` 1 for the blending ones).
void GlRenderer::drawList(const FrameView& f, const std::vector<Draw>& list, int blend) {
    useProgram(scene_);
    setAttributes(kSceneAttributes);
    for (const auto& d : list) {
        const MeshBuffers& buffers = gpu_->meshBuffers[d.primitive->buffers];
        if (buffers.vertices == 0 || buffers.indices == 0) {
            continue;
        }
        const auto& p = *d.primitive;
        const scene::Material* m = d.material;
        State state;
        state.blend = blend && m && m->alpha == scene::AlphaMode::Add ? 2 : blend;
        state.cull = !(m && m->doubleSided);
        state.depthWrite = blend == 0;
        setState(state);
        const DrawUniforms u = FramePlan::uniformsFor(*this, *this, f, d, blend);
        gl_.Uniform4fv(scene_.vertex, static_cast<GLsizei>(sizeof(VertexUniforms) / 16), reinterpret_cast<const GLfloat*>(&u.vertex.frame));
        gl_.Uniform4fv(scene_.fragment, static_cast<GLsizei>(sizeof(FragmentUniforms) / 16), reinterpret_cast<const GLfloat*>(&u.fragment));
        for (unsigned k = 0; k < u.samplers.size(); ++k) {
            bindTexture(k, textureFor(u.samplers[k].texture), u.samplers[k].sampler == SamplerKind::Clamped ? GL_CLAMP_TO_EDGE : GL_REPEAT);
        }
        // A posed primitive's vertices are its own; the others start at theirs in the shared buffer. OpenGL 2
        // has no base vertex to draw with, so the attributes start there.
        std::size_t start = 0;
        if (p.skinned) {
            poseSkinned(p, buffers, static_cast<std::size_t>(d.instance - instances_.data()));
        } else {
            start = static_cast<std::size_t>(p.firstVertex) * kVertexBytes;
            if (arrayBuffer_ != buffers.vertices) {
                gl_.BindBuffer(GL_ARRAY_BUFFER, buffers.vertices);
                arrayBuffer_ = buffers.vertices;
            }
        }
        gl_.VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, kVertexBytes, offset(start));
        gl_.VertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, kVertexBytes, offset(start + 3 * sizeof(float)));
        gl_.VertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, kVertexBytes, offset(start + 6 * sizeof(float)));
        gl_.VertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, kVertexBytes, offset(start + 8 * sizeof(float)));
        gl_.VertexAttribPointer(6, 2, GL_FLOAT, GL_FALSE, kVertexBytes, offset(start + 9 * sizeof(float)));
        gl_.VertexAttribPointer(7, 3, GL_FLOAT, GL_FALSE, kVertexBytes, offset(start + 11 * sizeof(float)));
        gl_.VertexAttribPointer(8, 3, GL_FLOAT, GL_FALSE, kVertexBytes, offset(start + 14 * sizeof(float)));
        if (elementBuffer_ != buffers.indices) {
            gl_.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffers.indices);
            elementBuffer_ = buffers.indices;
        }
        gl_.DrawElements(GL_TRIANGLES, static_cast<GLsizei>(p.count), GL_UNSIGNED_INT, offset(static_cast<std::size_t>(p.firstIndex) * 4));
    }
}

void GlRenderer::drawSprites(unsigned width, unsigned height) {
    std::vector<float>& v = scratch_;
    v.clear();
    v.reserve(sprites_.size() * 6 * kSpriteFloats);
    for (const auto& s : sprites_) {
        const float corners[6][4] = {{s.x0, s.y0, s.u0, s.v0}, {s.x1, s.y0, s.u1, s.v0}, {s.x1, s.y1, s.u1, s.v1},
                                     {s.x0, s.y0, s.u0, s.v0}, {s.x1, s.y1, s.u1, s.v1}, {s.x0, s.y1, s.u0, s.v1}};
        for (const auto& c : corners) {
            v.insert(v.end(), {c[0], c[1], c[2], c[3], s.color[0], s.color[1], s.color[2], s.color[3]});
        }
    }
    useProgram(ui_);
    const float screen[4] = {static_cast<float>(width), static_cast<float>(height), 0, 0};
    gl_.Uniform4fv(ui_.vertex, 1, screen);
    gl_.BindBuffer(GL_ARRAY_BUFFER, spriteVertices_);
    arrayBuffer_ = spriteVertices_;
    gl_.BufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(v.size() * sizeof(float)), v.data(), GL_STREAM_DRAW);
    setAttributes(kSpriteAttributes);
    constexpr GLsizei stride = kSpriteFloats * sizeof(float);
    gl_.VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, offset(0));
    gl_.VertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, offset(2 * sizeof(float)));
    gl_.VertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, stride, offset(4 * sizeof(float)));
    State sprites;
    sprites.blend = 1;
    sprites.depthTest = false;
    sprites.depthWrite = false;
    setState(sprites);
    for (std::size_t i = 0; i < sprites_.size();) {
        const int t = sprites_[i].texture;
        std::size_t j = i;
        while (j < sprites_.size() && sprites_[j].texture == t) {
            ++j;
        }
        const bool has = t >= 0 && static_cast<std::size_t>(t) < spriteTextures_.size() && spriteTextures_[static_cast<std::size_t>(t)].id != 0;
        if (!has && gpu_->white.id == 0) {
            i = j;
            continue;
        }
        bindTexture(0, has ? spriteTextures_[static_cast<std::size_t>(t)] : gpu_->white, GL_CLAMP_TO_EDGE);
        gl_.DrawArrays(GL_TRIANGLES, static_cast<GLint>(i * 6), static_cast<GLsizei>((j - i) * 6));
        i = j;
    }
}

// Records and draws one frame into the framebuffer bound now. False when it couldn't.
bool GlRenderer::render(unsigned width, unsigned height, const Camera& camera) {
    diag::Timer timer("record frame");
    collectGarbage();
    invalidateState();
    const FrameView f = FrameView::make(camera, width, height);
    // The neighborhood's water and its reflection, drawn first into a target of its own.
    const bool extras = drawScene_ && !instancesOnly_;
    const bool water = extras && gpu_->terrainIndexCount > 0 && gpu_->waterIndexCount > 0 && reflectionReady(width, height);
    if (water) {
        drawReflection(f, width, height);
    }
    gl_.Viewport(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
    gl_.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    gl_.DepthMask(GL_TRUE);
    gl_.ClearColor(instancesOnly_ ? 0.0f : clear_[0], instancesOnly_ ? 0.0f : clear_[1], instancesOnly_ ? 0.0f : clear_[2],
                   instancesOnly_ ? 0.0f : 1.0f);
    // Depth runs from 1 at the near plane to 0 at the far one (see Mat4::perspective).
    gl_.ClearDepth(0.0);
    // Only the part drawn to is cleared, as a picture drawn in a corner of the window (see drawOffscreen) leaves the rest alone.
    gl_.Enable(GL_SCISSOR_TEST);
    gl_.Scissor(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
    gl_.Clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    gl_.Disable(GL_SCISSOR_TEST);
    gl_.DepthRange(0.0, 1.0);
    gl_.FrontFace(GL_CCW);
    gl_.CullFace(GL_BACK);

    // The sky and the boundary curtain, the terrain, roads and imposters. A portrait shows its
    // instances alone, over a transparent background.
    if (extras && gpu_->skybox.id != 0) {
        drawSky(f, gpu_->skybox, 1);
    }
    if (extras && gpu_->curtainVertices != 0) {
        drawBoundary(f);
    }
    std::vector<Draw> opaque, layers, blended;
    FramePlan::collectDraws(*this, *this, f, opaque, layers, blended);
    drawList(f, opaque, 0);
    if (extras && gpu_->terrainVertices != 0) {
        if (gpu_->terrainIndexCount > 0) {
            drawTerrain(f, f.viewProjection, false);
        }
        if (!gpu_->roadRuns.empty()) {
            drawRoads(f);
        }
        drawImposters(f);
    }
    drawList(f, layers, 1);
    if (water) {
        drawWater(f, width, height);
    }
    drawList(f, blended, 1);
    if (!instancesOnly_ && !sprites_.empty()) {
        drawSprites(width, height);
    }
    return true;
}

} // namespace ts2::render
