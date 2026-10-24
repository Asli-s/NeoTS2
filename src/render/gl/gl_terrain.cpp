// The neighborhood: its terrain, roads and lot imposters, the sky behind a lot, and a lot's boundary curtain.
#include "diag/log.hpp"
#include "render/fixed_layout.hpp"
#include "render/frame_plan.hpp"
#include "render/gl/gl_renderer.hpp"
#include "render/image_levels.hpp"

#include <algorithm>
#include <set>

namespace ts2::render {

namespace {

const void* offsetOf(std::size_t bytes) {
    return reinterpret_cast<const void*>(bytes);
}

constexpr unsigned kMaxPaintLayers = 8;  // four to an alpha map's and a cells' texture, two of each

} // namespace

GLuint GlRenderer::uploadBuffer(GLenum target, const void* data, std::size_t bytes) {
    GLuint id = 0;
    gl_.GenBuffers(1, &id);
    gl_.BindBuffer(target, id);
    gl_.BufferData(target, static_cast<GLsizeiptr>(bytes), data, GL_STATIC_DRAW);
    forgetBindings();
    return id;
}

// A texture of these RGBA texels, clamped at its edges, without smaller levels.
GlRenderer::Texture GlRenderer::uploadRaw(unsigned width, unsigned height, const std::uint8_t* rgba, bool nearest) {
    Texture t;
    checkTexture(GL_TEXTURE_2D, width, height);
    t.wrap = GL_CLAMP_TO_EDGE;
    t.nearest = nearest;
    gl_.GenTextures(1, &t.id);
    gl_.ActiveTexture(GL_TEXTURE0);
    gl_.BindTexture(GL_TEXTURE_2D, t.id);
    forgetBindings();
    gl_.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    gl_.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, nearest ? GL_NEAREST : GL_LINEAR);
    gl_.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, nearest ? GL_NEAREST : GL_LINEAR);
    gl_.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl_.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl_.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, static_cast<GLsizei>(width), static_cast<GLsizei>(height), 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    return t;
}

// The terrain's grid, paints, where each shows, its light map and underwater gradient; its roads,
// imposters and the sky's triangle.
void GlRenderer::uploadTerrain(SceneGpu& gpu, const scene::PaintedTerrain& t) {
    const IndexedMesh grid = terrainGrid(t);
    if (fixed_) {
        // Without shaders the vertices carry the textures' coordinates, and the gradient is in their colours.
        const std::vector<float> vertices = fixedTerrainVertices(grid, t);
        gpu.terrainVertices = uploadBuffer(GL_ARRAY_BUFFER, vertices.data(), vertices.size() * sizeof(float));
        const std::vector<std::uint8_t> colors = fixedTerrainColors(grid, t);
        gpu.terrainColors = uploadBuffer(GL_ARRAY_BUFFER, colors.data(), colors.size());
    } else {
        gpu.terrainVertices = uploadBuffer(GL_ARRAY_BUFFER, grid.vertices.data(), grid.vertices.size() * sizeof(float));
    }
    if (!grid.indices.empty()) {  // every tile can be hidden (a lot's own ground stands in for it)
        gpu.terrainIndices = uploadBuffer(GL_ELEMENT_ARRAY_BUFFER, grid.indices.data(), grid.indices.size() * sizeof(std::uint32_t));
    }
    gpu.terrainIndexCount = static_cast<unsigned>(grid.indices.size());

    // Every paint texture is its own, and one that doesn't have the first one's size (as the SDL GPU
    // renderer's texture array needs them) is drawn white.
    const scene::Image white{1, 1, {255, 255, 255, 255}, {}};
    const scene::Image& first = complete(t.textures.front()) ? t.textures.front() : white;
    for (const auto& image : t.textures) {
        const bool fits = complete(image) && image.width == first.width && image.height == first.height;
        gpu.terrainPaints.push_back(uploadTexture(fits ? image : white, true));
    }
    // Which paint textures a cell shows, so the others aren't drawn.
    std::set<std::uint8_t> shown(t.cellTextures.begin(), t.cellTextures.end());
    gpu.terrainSlices.assign(shown.begin(), shown.end());
    if (fixed_) {
        // Each paint texture's weight over the map, which the pass for it multiplies it by.
        for (const unsigned slice : gpu.terrainSlices.empty() ? std::vector<std::uint8_t>{0} : gpu.terrainSlices) {
            const std::vector<std::uint8_t> weights = fixedPaintWeights(t, slice);
            gpu.terrainWeights.push_back(uploadRaw(t.alphaSize, t.alphaSize, weights.data(), false));
            // And the colours of its pass when it has one texture to draw with.
            const std::vector<std::uint8_t> colors = fixedSliceColors(grid, t, slice);
            gpu.terrainSliceColors.push_back(uploadBuffer(GL_ARRAY_BUFFER, colors.data(), colors.size()));
        }
    }

    const auto paints = static_cast<unsigned>(t.paints());
    if (paints > kMaxPaintLayers) {
        diag::warning("renderer: the terrain has more paints than OpenGL 2 draws; the rest are left out");
    }
    // Four paints' alpha maps share a texture, one in each channel. So do their cells' textures.
    const auto pack = [&](unsigned width, unsigned height, const std::uint8_t* maps, unsigned k) {
        const std::size_t texels = static_cast<std::size_t>(width) * height;
        std::vector<std::uint8_t> out(texels * 4, 0);
        for (unsigned c = 0; c < 4; ++c) {
            const unsigned layer = k * 4 + c;
            if (layer >= std::min(paints, kMaxPaintLayers)) {
                break;
            }
            for (std::size_t i = 0; i < texels; ++i) {
                out[i * 4 + c] = maps[layer * texels + i];
            }
        }
        return out;
    };
    for (unsigned k = 0; k < 2 && !fixed_; ++k) {
        const auto alphas = pack(t.alphaSize, t.alphaSize, t.alphas.data(), k);
        gpu.terrainAlphas[k] = uploadRaw(t.alphaSize, t.alphaSize, alphas.data(), false);
        const auto cells = pack(t.cellsX, t.cellsY, t.cellTextures.data(), k);
        gpu.terrainCells[k] = uploadRaw(t.cellsX, t.cellsY, cells.data(), true);
    }
    // The light map wraps: the game's lighting stage sets no addressing, leaving D3D's default. White (no change) when there is none.
    gpu.terrainLight = uploadTexture(complete(t.lightMap) ? t.lightMap : white, false);
    const scene::Image& gradient = complete(t.underwaterGradient) ? t.underwaterGradient : white;
    gpu.terrainGradient = uploadRaw(gradient.width, gradient.height, gradient.rgba.data(), false);
    gpu.terrainUniforms = {static_cast<float>(t.alphaSize), t.textureRepeat, static_cast<float>(t.cellTiles), static_cast<float>(paints),
                           static_cast<float>(t.cellsX), static_cast<float>(t.cellsY), t.lightMap.width ? t.lightMapScale : 1.0f, t.litFrom,
                           t.gradientDepth, 0, 0, 0};

    if (!t.roads.empty() && !t.roadTextures.empty()) {
        const RoadMesh roads = roadQuads(t);
        gpu.lightMapSize = t.lightMapScale * t.tileSize;
        const std::vector<float> roadVertices = fixed_ ? fixedRoadVertices(roads, gpu.lightMapSize) : roads.mesh.vertices;
        gpu.roadVertices = uploadBuffer(GL_ARRAY_BUFFER, roadVertices.data(), roadVertices.size() * sizeof(float));
        if (fixed_) {
            const std::vector<std::uint8_t> colors = fixedRoadColors(roads, t, gpu.lightMapSize);
            gpu.roadColors = uploadBuffer(GL_ARRAY_BUFFER, colors.data(), colors.size());
        }
        gpu.roadIndices = uploadBuffer(GL_ELEMENT_ARRAY_BUFFER, roads.mesh.indices.data(), roads.mesh.indices.size() * sizeof(std::uint32_t));
        gpu.roadRuns = roads.runs;
        for (const auto& image : t.roadTextures) {
            gpu.roadTextures.push_back(uploadTexture(complete(image) ? image : white, true, false));
        }
    }
    if (!t.imposters.empty()) {
        for (const auto& image : t.imposterTextures) {
            gpu.imposterTextures.push_back(uploadTexture(complete(image) ? image : white, false));
        }
        const ImposterMesh parts = imposterParts(t);
        gpu.imposterVertices = uploadBuffer(GL_ARRAY_BUFFER, parts.mesh.vertices.data(), parts.mesh.vertices.size() * sizeof(float));
        gpu.imposterIndices = uploadBuffer(GL_ELEMENT_ARRAY_BUFFER, parts.mesh.indices.data(), parts.mesh.indices.size() * sizeof(std::uint32_t));
        gpu.imposterDraws = parts.draws;
    }

    // The water: a sheet over the wet tiles, its ripples, and the sky it reflects.
    const IndexedMesh water = waterSheet(t);
    if (!water.indices.empty() && t.sky.size() == 6 && t.ripples.width) {
        gpu.waterVertices = uploadBuffer(GL_ARRAY_BUFFER, water.vertices.data(), water.vertices.size() * sizeof(float));
        gpu.waterIndices = uploadBuffer(GL_ELEMENT_ARRAY_BUFFER, water.indices.data(), water.indices.size() * sizeof(std::uint32_t));
        gpu.waterIndexCount = static_cast<unsigned>(water.indices.size());
        gpu.waterLevel = t.waterLevel;
        if (fixed_) {
            // No reflection to show: the water is the sky's average colour, tinted, as see-through as the reflection is strong.
            const std::array<float, 3> sky = averageColor(t.sky);
            gpu.waterFlat = {sky[0] * t.reflectionTint[0], sky[1] * t.reflectionTint[1], sky[2] * t.reflectionTint[2], t.reflectionStrength};
        } else {
            gpu.ripples = uploadTexture(complete(t.ripples) ? t.ripples : white, true);
            gpu.skyCube = uploadCube(t.sky, true);
        }
        gpu.waterUniforms = {{t.reflectionTint[0], t.reflectionTint[1], t.reflectionTint[2], t.reflectionStrength}, {0, 0},
                             t.reflectionOffset, t.rippleScale, t.rippleSpeed, 0, {0, 0}};
    }
}

// A sky cube map behind everything, looked up by each pixel's ray. `up` is -1 for the rays mirrored
// in the water.
void GlRenderer::drawSky(const FrameView& f, const Texture& cube, float up) {
    if (fixed_) {
        drawSkyFixed(f, cube);
        return;
    }
    useProgram(sky_);
    State state;
    state.depthTest = false;
    state.depthWrite = false;
    setState(state);
    bindTexture(0, const_cast<Texture&>(cube), GL_CLAMP_TO_EDGE);
    const std::array<float, 12> camera = skyCamera(f, up);
    gl_.Uniform4fv(sky_.fragment, 3, camera.data());
    gl_.BindBuffer(GL_ARRAY_BUFFER, skyVertices_);
    arrayBuffer_ = skyVertices_;
    setAttributes(1u << 0);
    attributes(0, 2, 2 * sizeof(float), 0);
    gl_.DrawArrays(GL_TRIANGLES, 0, 3);
}

// The boundary curtain, into depth only: nothing beyond it shows from inside.
void GlRenderer::drawBoundary(const FrameView& f) {
    if (fixed_) {
        drawBoundaryFixed(f);
        return;
    }
    useProgram(clip_);
    State state;
    state.cull = true;
    state.depthFunc = GL_GREATER;
    state.colorWrite = false;
    setState(state);
    gl_.Uniform4fv(clip_.vertex, 4, f.viewProjection.m.data());
    gl_.BindBuffer(GL_ARRAY_BUFFER, gpu_->curtainVertices);
    gl_.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, gpu_->curtainIndices);
    arrayBuffer_ = gpu_->curtainVertices;
    elementBuffer_ = gpu_->curtainIndices;
    setAttributes(1u << 0);
    attributes(0, 3, 3 * sizeof(float), 0);
    gl_.DrawElements(GL_TRIANGLES, 24, GL_UNSIGNED_INT, nullptr);
}

// The terrain, one pass for each paint texture a cell shows, the passes added up.
void GlRenderer::drawTerrain(const FrameView& f, const Mat4& viewProjection, bool reflected) {
    if (fixed_) {
        drawTerrainFixed(f);
        return;
    }
    useProgram(terrain_);
    (void)f;
    gl_.Uniform4fv(terrain_.vertex, 4, viewProjection.m.data());
    SceneGpu& g = *gpu_;
    bindTexture(1, g.terrainAlphas[0], GL_CLAMP_TO_EDGE);
    bindTexture(2, g.terrainAlphas[1], GL_CLAMP_TO_EDGE);
    bindTexture(3, g.terrainCells[0], GL_CLAMP_TO_EDGE, true);
    bindTexture(4, g.terrainCells[1], GL_CLAMP_TO_EDGE, true);
    bindTexture(5, g.terrainLight, GL_REPEAT);
    bindTexture(6, g.terrainGradient, GL_CLAMP_TO_EDGE);
    gl_.BindBuffer(GL_ARRAY_BUFFER, g.terrainVertices);
    gl_.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, g.terrainIndices);
    arrayBuffer_ = g.terrainVertices;
    elementBuffer_ = g.terrainIndices;
    setAttributes((1u << 0) | (1u << 1) | (1u << 2));
    constexpr GLsizei stride = kTerrainFloats * sizeof(float);
    attributes(0, 3, stride, 0);
    attributes(1, 3, stride, 3 * sizeof(float));
    attributes(2, 2, stride, 6 * sizeof(float));
    static const std::vector<std::uint8_t> kFirstSlice{0};
    const std::vector<std::uint8_t>& slices = g.terrainSlices.empty() ? kFirstSlice : g.terrainSlices;
    for (std::size_t pass = 0; pass < slices.size(); ++pass) {
        const std::size_t slice = slices[pass];
        if (slice >= g.terrainPaints.size()) {
            continue;
        }
        // The first pass replaces what was drawn before, the others add to it.
        State state;
        state.blend = pass == 0 ? 0 : 5;
        state.depthWrite = pass == 0;
        setState(state);
        TerrainUniforms u = g.terrainUniforms;
        u.reflected = reflected ? 1.0f : 0.0f;
        u.slice = static_cast<float>(slice);
        u.passAlpha = pass == 0 ? 1.0f : 0.0f;
        gl_.Uniform4fv(terrain_.fragment, static_cast<GLsizei>(sizeof(u) / 16), reinterpret_cast<const GLfloat*>(&u));
        bindTexture(0, g.terrainPaints[slice], GL_REPEAT);
        gl_.DrawElements(GL_TRIANGLES, static_cast<GLsizei>(g.terrainIndexCount), GL_UNSIGNED_INT, nullptr);
    }
}

void GlRenderer::drawRoads(const FrameView& f) {
    if (fixed_) {
        drawRoadsFixed(f);
        return;
    }
    useProgram(road_);
    SceneGpu& g = *gpu_;
    State state;
    state.blend = 1;
    setState(state);
    gl_.Uniform4fv(road_.vertex, 4, f.viewProjection.m.data());
    const float road[4] = {g.lightMapSize, 0, 0, 0};
    gl_.Uniform4fv(road_.fragment, 1, road);
    bindTexture(1, g.terrainLight, GL_REPEAT);
    gl_.BindBuffer(GL_ARRAY_BUFFER, g.roadVertices);
    gl_.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, g.roadIndices);
    arrayBuffer_ = g.roadVertices;
    elementBuffer_ = g.roadIndices;
    setAttributes((1u << 0) | (1u << 1) | (1u << 2));
    constexpr GLsizei stride = kRoadFloats * sizeof(float);
    attributes(0, 3, stride, 0);
    attributes(1, 2, stride, 3 * sizeof(float));
    attributes(2, 1, stride, 5 * sizeof(float));
    for (const auto& run : g.roadRuns) {
        if (run.texture >= g.roadTextures.size()) {
            continue;
        }
        bindTexture(0, g.roadTextures[run.texture], GL_CLAMP_TO_EDGE);
        gl_.DrawElements(GL_TRIANGLES, static_cast<GLsizei>(run.indexCount), GL_UNSIGNED_INT, offsetOf(static_cast<std::size_t>(run.firstIndex) * 4));
    }
}

void GlRenderer::drawImposters(const FrameView& f) {
    SceneGpu& g = *gpu_;
    if (g.imposterDraws.empty()) {
        return;
    }
    if (fixed_) {
        drawImpostersFixed(f);
        return;
    }
    using Kind = scene::PaintedTerrain::ImposterPart::Kind;
    useProgram(imposter_);
    gl_.Uniform4fv(imposter_.vertex, 4, f.viewProjection.m.data());
    gl_.BindBuffer(GL_ARRAY_BUFFER, g.imposterVertices);
    gl_.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, g.imposterIndices);
    arrayBuffer_ = g.imposterVertices;
    elementBuffer_ = g.imposterIndices;
    setAttributes((1u << 0) | (1u << 1) | (1u << 2));
    constexpr GLsizei stride = kImposterFloats * sizeof(float);
    struct PartUniforms {
        float tint[3];
        float cutoff, mode, light[3];
    };
    auto draw = [&](const State& state, const ImposterMesh::Part& d, int texture, GLint wrap, bool nearest, float cutoff, float mode) {
        setState(state);
        const PartUniforms u{{d.tint[0], d.tint[1], d.tint[2]}, cutoff, mode, {imposterLight_[0], imposterLight_[1], imposterLight_[2]}};
        gl_.Uniform4fv(imposter_.fragment, 2, reinterpret_cast<const GLfloat*>(&u));
        bindTexture(0, g.imposterTextures[static_cast<std::size_t>(texture)], wrap, nearest);
        // OpenGL 2 has no base vertex to draw with, so the attributes start at the part's.
        const std::size_t start = static_cast<std::size_t>(d.vertexOffset) * stride;
        attributes(0, 3, stride, start);
        attributes(1, 2, stride, start + 3 * sizeof(float));
        attributes(2, 2, stride, start + 5 * sizeof(float));
        gl_.DrawElements(GL_TRIANGLES, static_cast<GLsizei>(d.indexCount), GL_UNSIGNED_INT, offsetOf(static_cast<std::size_t>(d.firstIndex) * 4));
    };
    // Terrain: its light map multiplies what is under it, then its texture (alpha premultiplied)
    // goes over. Neither writes depth. Walls and slices: premultiplied, writing depth. Roofs:
    // opaque and two-sided.
    State light, terrain, wall, roof;
    light.blend = 3;
    light.cull = true;
    light.depthWrite = false;
    terrain.blend = 4;
    terrain.cull = true;
    terrain.depthWrite = false;
    wall.blend = 4;
    wall.cull = true;
    // The terrain first (its shader layer comes before the others'): light map, then texture.
    for (const auto& d : g.imposterDraws) {
        if (d.kind != Kind::Terrain) {
            continue;
        }
        if (d.lightMap >= 0) {
            draw(light, d, d.lightMap, GL_REPEAT, false, 0, 0);
        }
        draw(terrain, d, static_cast<int>(d.texture), GL_CLAMP_TO_EDGE, false, 0, 1);
    }
    for (const auto& d : g.imposterDraws) {
        switch (d.kind) {
        case Kind::Wall:
            draw(wall, d, static_cast<int>(d.texture), GL_REPEAT, true, 64.0f / 255, 1);
            break;
        case Kind::Slice:
            draw(wall, d, static_cast<int>(d.texture), GL_REPEAT, true, 127.0f / 255, 1);
            break;
        case Kind::Roof:
            draw(roof, d, static_cast<int>(d.texture), GL_CLAMP_TO_EDGE, false, -1, 2);
            break;
        case Kind::Terrain:
            break;
        }
    }
}

void GlRenderer::drawWater(const FrameView& f, unsigned width, unsigned height) {
    if (fixed_) {
        drawWaterFixed(f);
        return;
    }
    useProgram(water_);
    SceneGpu& g = *gpu_;
    State state;
    state.blend = 1;
    state.depthWrite = false;
    setState(state);
    gl_.Uniform4fv(water_.vertex, 4, f.viewProjection.m.data());
    WaterUniforms w = g.waterUniforms;
    w.screen[0] = static_cast<float>(width);
    w.screen[1] = static_cast<float>(height);
    w.time = time_;
    gl_.Uniform4fv(water_.fragment, static_cast<GLsizei>(sizeof(w) / 16), reinterpret_cast<const GLfloat*>(&w));
    bindTexture(0, reflection_, GL_CLAMP_TO_EDGE);
    bindTexture(1, g.ripples, GL_REPEAT);
    gl_.BindBuffer(GL_ARRAY_BUFFER, g.waterVertices);
    gl_.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, g.waterIndices);
    arrayBuffer_ = g.waterVertices;
    elementBuffer_ = g.waterIndices;
    setAttributes((1u << 0) | (1u << 1));
    constexpr GLsizei stride = kWaterFloats * sizeof(float);
    attributes(0, 3, stride, 0);
    attributes(1, 2, stride, 3 * sizeof(float));
    gl_.DrawElements(GL_TRIANGLES, static_cast<GLsizei>(g.waterIndexCount), GL_UNSIGNED_INT, nullptr);
}

void GlRenderer::releaseReflection() {
    if (gl_.framebuffers) {
        if (reflectionFramebuffer_) gl_.DeleteFramebuffersEXT(1, &reflectionFramebuffer_);
        if (reflectionDepth_) gl_.DeleteRenderbuffersEXT(1, &reflectionDepth_);
    }
    release(reflection_);
    reflectionFramebuffer_ = reflectionDepth_ = 0;
    reflectionWidth_ = reflectionHeight_ = 0;
}

// Makes the water's reflection target at the size drawn at. False when it couldn't be made.
bool GlRenderer::reflectionReady(unsigned width, unsigned height) {
    if (!gl_.framebuffers || width > static_cast<unsigned>(gl_.maxTextureSize) || height > static_cast<unsigned>(gl_.maxTextureSize)) {
        return false;
    }
    if (reflectionFramebuffer_ != 0 && reflectionWidth_ == width && reflectionHeight_ == height) {
        return true;
    }
    releaseReflection();
    reflection_.wrap = GL_CLAMP_TO_EDGE;
    gl_.GenTextures(1, &reflection_.id);
    gl_.ActiveTexture(GL_TEXTURE0);
    gl_.BindTexture(GL_TEXTURE_2D, reflection_.id);
    bound_ = {};
    gl_.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl_.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl_.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl_.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl_.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, static_cast<GLsizei>(width), static_cast<GLsizei>(height), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    gl_.GenRenderbuffersEXT(1, &reflectionDepth_);
    gl_.BindRenderbufferEXT(GL_RENDERBUFFER_EXT, reflectionDepth_);
    gl_.RenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_DEPTH_COMPONENT24, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
    gl_.GenFramebuffersEXT(1, &reflectionFramebuffer_);
    const GLuint before = framebuffer_;
    bindFramebuffer(reflectionFramebuffer_);
    gl_.FramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, reflection_.id, 0);
    gl_.FramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, reflectionDepth_);
    const bool complete = gl_.CheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT) == GL_FRAMEBUFFER_COMPLETE_EXT;
    bindFramebuffer(before);
    if (!complete) {
        releaseReflection();
        return false;
    }
    reflectionWidth_ = width;
    reflectionHeight_ = height;
    return true;
}

// The neighborhood water's reflection: the sky and the terrain above the water, mirrored in it.
void GlRenderer::drawReflection(const FrameView& f, unsigned width, unsigned height) {
    const GLuint before = framebuffer_;
    bindFramebuffer(reflectionFramebuffer_);
    gl_.Viewport(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
    gl_.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    gl_.DepthMask(GL_TRUE);
    gl_.ClearColor(0, 0, 0, 0);
    gl_.ClearDepth(0.0);
    gl_.Clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    state_.reset();
    // The sky, by each pixel's ray mirrored in the water.
    drawSky(f, gpu_->skyCube, -1);
    // The terrain, mirrored about the water's surface.
    Mat4 flip;
    flip.m = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1, 2 * gpu_->waterLevel, 0, 0, 0, 1};
    drawTerrain(f, f.viewProjection * flip, true);
    bindFramebuffer(before);
    state_.reset();
}

} // namespace ts2::render
