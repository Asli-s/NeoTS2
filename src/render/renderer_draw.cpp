#include "render/renderer.hpp"
#include "render/renderer_internals.hpp"

#include "diag/timing.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <limits>
#include <utility>

namespace ts2::render {

using namespace render_detail;

namespace {

constexpr std::size_t kObjectLights = 8;  // as many as the game's shaders take

struct VertexUniforms {
    float viewProjection[16];
    float model[16];
    float ambient[4];  // w: 1 when lit as the game lights objects (see scene::ObjectLighting), 3 as its lot skirt
    float lightDirections[kObjectLights][4];  // w: a point light's range squared (0: directional)
    float lightColors[kObjectLights][4];
    float lightPositions[kObjectLights][4];   // a point light's position and falloff
    float lightCones[kObjectLights][4];       // a spot light's edge cosine, 1 / (inner - edge) cosines, 1
    float thumbnail[4];  // the material's diffuse and the thumbnail scale (see scene::ObjectLighting)
    std::int32_t pixelLights[4];  // how many lights are lit per pixel through the normal map, and which
    float eye[4];                 // the camera's position
    float specular[4];            // the material's specular scale and power (0: none)
};

struct SkinnedUniforms {
    VertexUniforms frame;
    std::uint32_t jointBase;
    std::uint32_t padding[3];
};

struct FragmentUniforms {
    float color[4];
    float sunDirection[4];
    float hasTexture;
    float alphaCutoff;
    float unlit;
    float padding;
    float lightMap[4];  // x: the gain on the light map (0: none), y: 1 to leave the material's colour out, z: 1 for terrain
    float pixelLightColors[3][4];  // the colours of the lights lit per pixel
    float pixelLightCount;         // how many (0: none)
    float normalMapped;            // 1: the per-pixel normal from the normal map, 0: straight out
    float simLighting;             // 1: lit as the game's Sim materials (see scene::Material)
    float lightMapFade;            // how much of the light map is the new one (see Renderer::setLightMapFade)
    float envCube[4];              // the environment cube's colour scale, w: 1 when on (see scene::Material)
    // The camera's right, up and forward, right's w 1 to look the cube up in the camera's space.
    float cameraRight[4];
    float cameraUp[4];
    float cameraForward[4];
    float emissive[4];  // added to the light before the texture (see scene::Material), w: 1 when on
};

// The shaders' constant buffers pack in 16-byte rows, so these must match them field for field.
static_assert(sizeof(VertexUniforms) % 16 == 0 && offsetof(VertexUniforms, eye) == 688 && offsetof(VertexUniforms, specular) == 704);
static_assert(offsetof(SkinnedUniforms, jointBase) == 720 && sizeof(SkinnedUniforms) % 16 == 0);
static_assert(sizeof(FragmentUniforms) % 16 == 0 && offsetof(FragmentUniforms, lightMap) == 48 &&
              offsetof(FragmentUniforms, envCube) == 128 && offsetof(FragmentUniforms, emissive) == 192);

enum class InView { Outside, Inside, Partly };

// Where a model-space box lies against the view of `clip` (projection * view * model). It is
// outside when all eight corners are outside the same side of the clip volume (-w <= x, y <= w,
// 0 <= z <= w), so none of its triangles can show, and inside when no corner is outside any side.
InView boxInView(const Mat4& clip, const std::array<float, 3>& low, const std::array<float, 3>& high) {
    // The low corner in clip space, and the box's three edges from it.
    float base[4], edge[3][4];
    for (int r = 0; r < 4; ++r) {
        const float* row = &clip.m[static_cast<std::size_t>(r) * 4];
        base[r] = row[0] * low[0] + row[1] * low[1] + row[2] * low[2] + row[3];
        for (int k = 0; k < 3; ++k) {
            edge[k][r] = row[k] * (high[k] - low[k]);
        }
    }
    int outside[6] = {};
    for (int c = 0; c < 8; ++c) {
        float p[4];
        for (int r = 0; r < 4; ++r) {
            p[r] = base[r] + (c & 1 ? edge[0][r] : 0) + (c & 2 ? edge[1][r] : 0) + (c & 4 ? edge[2][r] : 0);
        }
        outside[0] += p[0] < -p[3];
        outside[1] += p[0] > p[3];
        outside[2] += p[1] < -p[3];
        outside[3] += p[1] > p[3];
        outside[4] += p[2] < 0;
        outside[5] += p[2] > p[3];
    }
    if (std::any_of(std::begin(outside), std::end(outside), [](int n) { return n == 8; })) {
        return InView::Outside;
    }
    return std::all_of(std::begin(outside), std::end(outside), [](int n) { return n == 0; }) ? InView::Inside : InView::Partly;
}

// How the game sorts a blended surface: the view depth of its box's nearest corner, a corner at or
// behind the camera counting as on the near plane. `clip`'s last row gives the view depth (the
// projection's w).
float nearestDepth(const Mat4& clip, const std::array<float, 3>& low, const std::array<float, 3>& high, float nearZ) {
    float nearest = std::numeric_limits<float>::max();
    for (int c = 0; c < 8; ++c) {
        const float x = c & 1 ? high[0] : low[0], y = c & 2 ? high[1] : low[1], z = c & 4 ? high[2] : low[2];
        const float w = clip.m[12] * x + clip.m[13] * y + clip.m[14] * z + clip.m[15];
        nearest = std::min(nearest, w > 1e-5f ? w : nearZ);
    }
    return nearest;
}

} // namespace

void Renderer::drawImposters(SDL_GPUCommandBuffer* cmd, SDL_GPURenderPass* pass, const Mat4& viewProjection) {
    if (gpu_->imposterDraws.empty()) {
        return;
    }
    using Kind = scene::PaintedTerrain::ImposterPart::Kind;
    SDL_GPUBufferBinding vertices{gpu_->imposterVertices, 0};
    SDL_GPUBufferBinding indices{gpu_->imposterIndices, 0};
    struct PartUniforms {
        float tint[3];
        float cutoff, mode, light[3];
    };
    auto draw = [&](SDL_GPUGraphicsPipeline* pipeline, const ImposterDraw& d, SDL_GPUTexture* texture, SDL_GPUSampler* sampler,
                    float cutoff, float mode) {
        SDL_BindGPUGraphicsPipeline(pass, pipeline);
        SDL_PushGPUVertexUniformData(cmd, 0, viewProjection.m.data(), 16 * sizeof(float));
        const PartUniforms u{{d.tint[0], d.tint[1], d.tint[2]}, cutoff, mode, {imposterLight_[0], imposterLight_[1], imposterLight_[2]}};
        SDL_PushGPUFragmentUniformData(cmd, 0, &u, sizeof(u));
        SDL_GPUTextureSamplerBinding binding{texture, sampler};
        SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
        SDL_BindGPUVertexBuffers(pass, 0, &vertices, 1);
        SDL_BindGPUIndexBuffer(pass, &indices, SDL_GPU_INDEXELEMENTSIZE_32BIT);
        SDL_DrawGPUIndexedPrimitives(pass, d.indexCount, 1, d.firstIndex, d.vertexOffset, 0);
    };
    // The terrain first (its shader layer comes before the others'): light map, then texture.
    for (const auto& d : gpu_->imposterDraws) {
        if (d.kind != Kind::Terrain) {
            continue;
        }
        if (d.lightMap) {
            draw(imposterLightPipeline_, d, d.lightMap, sampler_, 0, 0);
        }
        draw(imposterTerrainPipeline_, d, d.texture, clampedMips_, 0, 1);
    }
    for (const auto& d : gpu_->imposterDraws) {
        switch (d.kind) {
        case Kind::Wall:
            draw(imposterWallPipeline_, d, d.texture, pointSampler_, 64.0f / 255, 1);
            break;
        case Kind::Slice:
            draw(imposterWallPipeline_, d, d.texture, pointSampler_, 127.0f / 255, 1);
            break;
        case Kind::Roof:
            draw(imposterRoofPipeline_, d, d.texture, clampedMips_, -1, 2);
            break;
        case Kind::Terrain:
            break;
        }
    }
}

void Renderer::show(std::vector<std::uint8_t> visible) {
    visible_ = std::move(visible);
}

bool Renderer::draw(const Camera& camera) {
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    if (cmd == nullptr) {
        return false;
    }
    SDL_GPUTexture* target = nullptr;
    Uint32 width = 0, height = 0;
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd, window_, &target, &width, &height) || target == nullptr) {
        SDL_SubmitGPUCommandBuffer(cmd);
        return false;
    }
    const bool drawn = render(cmd, target, width, height, camera);
    SDL_SubmitGPUCommandBuffer(cmd);
    return drawn;
}

bool Renderer::drawToFile(const Camera& camera, unsigned width, unsigned height, const std::filesystem::path& path) {
    scene::Image image;
    if (!drawOffscreen(camera, width, height, image)) {
        return false;
    }
    SDL_Surface* surface = SDL_CreateSurfaceFrom(static_cast<int>(width), static_cast<int>(height), SDL_PIXELFORMAT_RGBA32,
                                                 image.rgba.data(), static_cast<int>(width * 4));
    const bool saved = surface != nullptr && SDL_SavePNG(surface, path.string().c_str());
    SDL_DestroySurface(surface);
    return saved;
}

std::optional<scene::Image> Renderer::drawInstancesToImage(const Camera& camera, unsigned width, unsigned height,
                                                           const scene::ObjectLighting& lighting) {
    // Back to the scene's own drawing afterwards, even if drawing throws.
    struct Restore {
        Renderer& r;
        std::optional<scene::ObjectLighting> lighting;
        ~Restore() {
            r.objectLighting_ = std::move(lighting);
            r.instancesOnly_ = false;
        }
    } restore{*this, std::exchange(objectLighting_, lighting)};
    instancesOnly_ = true;
    scene::Image image;
    const bool drawn = drawOffscreen(camera, width, height, image);
    return drawn ? std::optional<scene::Image>(std::move(image)) : std::nullopt;
}

bool Renderer::drawOffscreen(const Camera& camera, unsigned width, unsigned height, scene::Image& out) {
    if (width == 0 || height == 0 || width > kMaxOffscreen || height > kMaxOffscreen) {
        return false;
    }
    const SDL_GPUTextureFormat format = SDL_GetGPUSwapchainTextureFormat(device_, window_);
    // The pipelines draw in the window's format. Only 8-bit RGBA or BGRA can be read back as bytes.
    if (format != SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM && format != SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM) {
        return false;
    }
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = format;
    info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = width;
    info.height = height;
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    SDL_GPUTexture* target = SDL_CreateGPUTexture(device_, &info);
    SDL_GPUTransferBufferCreateInfo transferInfo{};
    transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    transferInfo.size = width * height * 4;
    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device_, &transferInfo);
    SDL_GPUCommandBuffer* cmd = target && transfer ? SDL_AcquireGPUCommandBuffer(device_) : nullptr;
    if (cmd == nullptr) {
        SDL_ReleaseGPUTransferBuffer(device_, transfer);
        SDL_ReleaseGPUTexture(device_, target);
        return false;
    }
    if (!render(cmd, target, width, height, camera)) {
        SDL_CancelGPUCommandBuffer(cmd);
        SDL_ReleaseGPUTransferBuffer(device_, transfer);
        SDL_ReleaseGPUTexture(device_, target);
        return false;
    }
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureRegion from{};
    from.texture = target;
    from.w = width;
    from.h = height;
    from.d = 1;
    SDL_GPUTextureTransferInfo to{};
    to.transfer_buffer = transfer;
    to.pixels_per_row = width;
    to.rows_per_layer = height;
    SDL_DownloadFromGPUTexture(copy, &from, &to);
    SDL_EndGPUCopyPass(copy);
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (fence == nullptr) {
        SDL_ReleaseGPUTransferBuffer(device_, transfer);
        SDL_ReleaseGPUTexture(device_, target);
        return false;
    }
    SDL_WaitForGPUFences(device_, true, &fence, 1);
    SDL_ReleaseGPUFence(device_, fence);

    const auto* pixels = static_cast<const std::uint8_t*>(SDL_MapGPUTransferBuffer(device_, transfer, false));
    if (pixels != nullptr) {
        out.width = width;
        out.height = height;
        out.rgba.assign(pixels, pixels + std::size_t{width} * height * 4);
        if (format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM) {
            for (std::size_t i = 0; i + 3 < out.rgba.size(); i += 4) {
                std::swap(out.rgba[i], out.rgba[i + 2]);
            }
        }
        SDL_UnmapGPUTransferBuffer(device_, transfer);
    }
    SDL_ReleaseGPUTransferBuffer(device_, transfer);
    SDL_ReleaseGPUTexture(device_, target);
    return pixels != nullptr;
}

SDL_GPUTexture* Renderer::depthFor(DepthBuffer& depth, unsigned width, unsigned height) {
    if (depth.texture == nullptr || depth.width != width || depth.height != height) {
        if (depth.texture) {
            SDL_ReleaseGPUTexture(device_, depth.texture);
        }
        SDL_GPUTextureCreateInfo info{};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
        info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
        info.width = width;
        info.height = height;
        info.layer_count_or_depth = 1;
        info.num_levels = 1;
        depth.texture = SDL_CreateGPUTexture(device_, &info);
        depth.width = depth.texture ? width : 0;
        depth.height = depth.texture ? height : 0;
    }
    return depth.texture;
}

// One frame being recorded: where it goes and how it is seen.
struct Renderer::Frame {
    SDL_GPUCommandBuffer* cmd = nullptr;
    SDL_GPURenderPass* pass = nullptr;  // the frame's main pass, once begun
    unsigned width = 0, height = 0;
    const Camera* camera = nullptr;
    float aspect = 1, fovY = 0, nearZ = 0;
    Mat4 viewProjection;
    // The sky, the boundary curtain, the terrain, roads and imposters. A portrait shows its instances
    // alone, over a transparent background.
    bool extras = false;
    bool water = false;  // the neighborhood's water and its reflection
    Vec3 sun{};
};

// A primitive to draw, with what its draw needs.
struct Renderer::Draw {
    const GpuPrimitive* primitive;
    const scene::Instance* instance;
    const scene::Material* material;
    int layer;    // the material's draw layer
    float depth;  // blended: the game's sort depth (nearestDepth)
    float sun;    // how much of the sun reaches it
};

// What one draw pushes and binds.
struct Renderer::DrawUniforms {
    SkinnedUniforms vertex{};
    FragmentUniforms fragment{};
    std::array<SDL_GPUTextureSamplerBinding, 6> samplers{};
};

bool Renderer::render(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* target, unsigned width, unsigned height,
                      const Camera& camera) {
    diag::Timer timer("record frame");
    SDL_GPUTexture* depthTexture = depthFor(instancesOnly_ ? portraitDepth_ : windowDepth_, width, height);
    if (depthTexture == nullptr) {
        return false;
    }

    uploadSpriteVertices(cmd, width, height);
    uploadJoints(cmd);

    Frame f;
    f.cmd = cmd;
    f.width = width;
    f.height = height;
    f.camera = &camera;
    f.aspect = height ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;
    f.fovY = camera.fovX > 0 ? 2 * std::atan(std::tan(camera.fovX / 2) / f.aspect) : camera.fovY;
    f.nearZ = camera.nearPlane > 0 ? camera.nearPlane : camera.farPlane / 10000;
    f.viewProjection = Mat4::perspective(f.fovY, f.aspect, f.nearZ, camera.farPlane) *
                       Mat4::lookAt(camera.eye, camera.target, {0, 0, 1});
    f.extras = drawScene_ && !instancesOnly_;
    f.water = f.extras && gpu_->terrainIndexCount > 0 && gpu_->waterIndexCount > 0 && reflectionReady(width, height);
    f.sun = normalize({0.4f, -0.5f, 0.75f});

    if (f.water) {
        drawReflection(f);
    }

    SDL_GPUColorTargetInfo color{};
    color.texture = target;
    color.clear_color = instancesOnly_ ? SDL_FColor{0, 0, 0, 0} : SDL_FColor{clear_[0], clear_[1], clear_[2], 1.0f};
    color.load_op = SDL_GPU_LOADOP_CLEAR;
    color.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPUDepthStencilTargetInfo depth{};
    depth.texture = depthTexture;
    depth.clear_depth = 0;
    depth.load_op = SDL_GPU_LOADOP_CLEAR;
    depth.store_op = SDL_GPU_STOREOP_DONT_CARE;
    depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    f.pass = SDL_BeginGPURenderPass(cmd, &color, 1, &depth);
    // A lot's sky, behind everything (the game's skybox screen effect).
    if (f.extras && gpu_->skybox != nullptr) {
        drawSky(f, f.pass, gpu_->skybox, 1);
    }
    if (f.extras && gpu_->clipVertices != nullptr) {
        drawBoundary(f);
    }

    std::vector<Draw> opaque, layers, blended;
    collectDraws(f, opaque, layers, blended);
    drawList(f, opaque, 0);
    if (f.extras && gpu_->terrainVertices) {
        if (gpu_->terrainIndexCount > 0) {
            drawTerrain(f, f.pass, f.viewProjection, false);
        }
        if (gpu_->roadIndexCount > 0) {
            drawRoads(f);
        }
        drawImposters(cmd, f.pass, f.viewProjection);
    }
    drawList(f, layers, 1);
    if (f.water) {
        drawWater(f);
    }
    drawList(f, blended, 1);
    if (!instancesOnly_ && !sprites_.empty() && spriteVertices_) {
        drawSprites(f);
    }
    SDL_EndGPURenderPass(f.pass);
    return true;
}

// Makes the water's reflection textures at the size drawn at. False when they couldn't be made (out of
// video memory), to be tried again next frame.
bool Renderer::reflectionReady(unsigned width, unsigned height) {
    if (reflection_ == nullptr || reflectionDepth_ == nullptr || reflectionWidth_ != width || reflectionHeight_ != height) {
        for (auto** t : {&reflection_, &reflectionDepth_}) {
            if (*t) {
                SDL_ReleaseGPUTexture(device_, *t);
            }
        }
        SDL_GPUTextureCreateInfo info{};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = SDL_GetGPUSwapchainTextureFormat(device_, window_);
        info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        info.width = width;
        info.height = height;
        info.layer_count_or_depth = 1;
        info.num_levels = 1;
        reflection_ = SDL_CreateGPUTexture(device_, &info);
        info.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
        info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
        reflectionDepth_ = SDL_CreateGPUTexture(device_, &info);
        if (reflection_ && reflectionDepth_) {
            reflectionWidth_ = width;
            reflectionHeight_ = height;
        }
    }
    return reflection_ && reflectionDepth_;
}

// The neighborhood water's reflection: the sky and the terrain above the water, mirrored in it.
void Renderer::drawReflection(const Frame& f) {
    SDL_GPUColorTargetInfo rc{};
    rc.texture = reflection_;
    rc.load_op = SDL_GPU_LOADOP_CLEAR;
    rc.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPUDepthStencilTargetInfo rd{};
    rd.texture = reflectionDepth_;
    rd.clear_depth = 0;
    rd.load_op = SDL_GPU_LOADOP_CLEAR;
    rd.store_op = SDL_GPU_STOREOP_DONT_CARE;
    rd.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    rd.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    SDL_GPURenderPass* mirror = SDL_BeginGPURenderPass(f.cmd, &rc, 1, &rd);
    // The sky, by each pixel's ray mirrored in the water.
    drawSky(f, mirror, gpu_->skyCube, -1);
    // The terrain, mirrored about the water's surface.
    Mat4 flip;
    flip.m = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1, 2 * gpu_->waterLevel, 0, 0, 0, 1};
    drawTerrain(f, mirror, f.viewProjection * flip, true);
    SDL_EndGPURenderPass(mirror);
}

// A sky cube map behind everything, looked up by each pixel's ray. `up` is -1 for the rays mirrored
// in the water.
void Renderer::drawSky(const Frame& f, SDL_GPURenderPass* on, SDL_GPUTexture* cube, float up) {
    const Camera& camera = *f.camera;
    const Vec3 forward = normalize(sub(camera.target, camera.eye));
    const Vec3 s = normalize(cross(forward, {0, 0, 1}));
    const Vec3 u = cross(s, forward);
    const float ty = std::tan(f.fovY / 2), tx = ty * f.aspect;
    const float skyCamera[12] = {s[0] * tx, s[1] * tx, s[2] * tx, 0, u[0] * ty, u[1] * ty, u[2] * ty, 0, forward[0], forward[1], forward[2], up};
    SDL_BindGPUGraphicsPipeline(on, skyPipeline_);
    SDL_PushGPUFragmentUniformData(f.cmd, 0, skyCamera, sizeof(skyCamera));
    SDL_GPUTextureSamplerBinding skyBinding{cube, sampler_};
    SDL_BindGPUFragmentSamplers(on, 0, &skyBinding, 1);
    SDL_DrawGPUPrimitives(on, 3, 1, 0, 0);
}

// The boundary curtain, into depth only: nothing beyond it shows from inside.
void Renderer::drawBoundary(const Frame& f) {
    SDL_BindGPUGraphicsPipeline(f.pass, clipPipeline_);
    SDL_PushGPUVertexUniformData(f.cmd, 0, f.viewProjection.m.data(), 16 * sizeof(float));
    SDL_GPUBufferBinding vertices{gpu_->clipVertices, 0};
    SDL_BindGPUVertexBuffers(f.pass, 0, &vertices, 1);
    SDL_GPUBufferBinding indices{gpu_->clipIndices, 0};
    SDL_BindGPUIndexBuffer(f.pass, &indices, SDL_GPU_INDEXELEMENTSIZE_32BIT);
    SDL_DrawGPUIndexedPrimitives(f.pass, 24, 1, 0, 0, 0);
}

void Renderer::drawTerrain(const Frame& f, SDL_GPURenderPass* on, const Mat4& viewProjection, bool reflected) {
    SDL_BindGPUGraphicsPipeline(on, terrainPipeline_);
    SDL_PushGPUVertexUniformData(f.cmd, 0, viewProjection.m.data(), 16 * sizeof(float));
    TerrainUniforms u = gpu_->terrainUniforms;
    u.reflected = reflected ? 1.0f : 0.0f;
    SDL_PushGPUFragmentUniformData(f.cmd, 0, &u, sizeof(u));
    // The light map wraps: the game's lighting stage sets no addressing, leaving D3D's default.
    SDL_GPUTextureSamplerBinding bindings[5] = {{gpu_->terrainPaints, sampler_},      {gpu_->terrainAlphas, spriteSampler_},
                                                {gpu_->terrainCells, spriteSampler_}, {gpu_->terrainLight, sampler_},
                                                {gpu_->terrainGradient, spriteSampler_}};
    SDL_BindGPUFragmentSamplers(on, 0, bindings, 5);
    SDL_GPUBufferBinding vertices{gpu_->terrainVertices, 0};
    SDL_BindGPUVertexBuffers(on, 0, &vertices, 1);
    SDL_GPUBufferBinding indices{gpu_->terrainIndices, 0};
    SDL_BindGPUIndexBuffer(on, &indices, SDL_GPU_INDEXELEMENTSIZE_32BIT);
    SDL_DrawGPUIndexedPrimitives(on, gpu_->terrainIndexCount, 1, 0, 0, 0);
}

// The shown instances' primitives in view, split by how they are drawn (opaque, layered, blended)
// and in the game's order: by layer, and blended surfaces of a layer from the farthest to the nearest.
void Renderer::collectDraws(const Frame& f, std::vector<Draw>& opaque, std::vector<Draw>& layers, std::vector<Draw>& blended) const {
    std::vector<float> depths;
    for (std::size_t i = 0; drawScene_ && i < instances_.size(); ++i) {
        const auto& instance = instances_[i];
        if (instance.mesh < 0 || static_cast<std::size_t>(instance.mesh) >= gpu_->meshes.size() || (i < visible_.size() && !visible_[i])) {
            continue;
        }
        Mat4 model;
        std::copy_n(instance.transform.m.begin(), 12, model.m.begin());
        const Mat4 clip = f.viewProjection * model;
        const GpuMesh& mesh = gpu_->meshes[static_cast<std::size_t>(instance.mesh)];
        const InView meshInView = mesh.anyCullable ? boxInView(clip, mesh.low, mesh.high) : InView::Inside;
        const std::size_t firstBlended = blended.size();
        for (const auto& p : mesh.primitives) {
            if (p.skinned && (i >= jointBase_.size() || jointBase_[i] < 0 || !jointBuffer_ || !gpu_->meshBuffers[p.buffers].skin)) {
                continue;
            }
            if (p.cullable && meshInView != InView::Inside &&
                (meshInView == InView::Outside || boxInView(clip, p.low, p.high) == InView::Outside)) {
                continue;
            }
            int material = p.material;
            if (i < instanceMaterials_.size() && p.source < instanceMaterials_[i].size()) {
                material = instanceMaterials_[i][p.source];
                if (material == scene::kHiddenMaterial) {
                    continue;
                }
            }
            const scene::Material* m =
                material >= 0 && static_cast<std::size_t>(material) < materials_.size() ? &materials_[static_cast<std::size_t>(material)] : nullptr;
            const auto alpha = m ? m->alpha : scene::AlphaMode::Opaque;
            const bool blends = alpha == scene::AlphaMode::Blend || alpha == scene::AlphaMode::Add;
            const float sortDepth = blends && p.hasBox ? nearestDepth(clip, p.low, p.high, f.nearZ) : clip.m[15];
            (blends ? blended : alpha == scene::AlphaMode::Layer ? layers : opaque)
                .push_back({&p, &instance, m, m ? m->layer : 0, sortDepth, p.source < instance.sun.size() ? instance.sun[p.source] : 1.0f});
        }
        // The game draws an object's blended surfaces of one layer from its model's last to its first
        // (a Sim's hair underside first, the outer strands last): their depths are handed out again,
        // the farthest to the first, so they keep that order among other objects' surfaces.
        const auto own = blended.begin() + static_cast<std::ptrdiff_t>(firstBlended);
        std::reverse(own, blended.end());
        std::stable_sort(own, blended.end(), [](const Draw& a, const Draw& b) { return a.layer < b.layer; });
        for (auto run = own; run != blended.end();) {
            const auto end = std::find_if(run, blended.end(), [&](const Draw& d) { return d.layer != run->layer; });
            depths.clear();
            for (auto it = run; it != end; ++it) {
                depths.push_back(it->depth);
            }
            std::sort(depths.begin(), depths.end(), std::greater<>());
            for (std::size_t k = 0; run != end; ++run, ++k) {
                run->depth = depths[k];
            }
        }
    }
    std::stable_sort(opaque.begin(), opaque.end(), [](const Draw& a, const Draw& b) { return a.layer < b.layer; });
    std::stable_sort(blended.begin(), blended.end(),
                     [](const Draw& a, const Draw& b) { return a.layer != b.layer ? a.layer < b.layer : a.depth > b.depth; });
}

// Draws primitives with the scene pipelines (`blend` 1 for the blending ones).
void Renderer::drawList(const Frame& f, const std::vector<Draw>& list, int blend) {
    // Each binding SDL is given takes new GPU descriptors, so textures and joints are bound only when
    // they change (and again after a pipeline switch).
    SDL_GPUGraphicsPipeline* bound = nullptr;
    std::array<SDL_GPUTextureSamplerBinding, 6> boundSamplers{};
    bool jointsBound = false;
    const MeshBuffers* boundBuffers = nullptr;
    for (const auto& d : list) {
        const MeshBuffers& buffers = gpu_->meshBuffers[d.primitive->buffers];
        if (buffers.vertices == nullptr || buffers.indices == nullptr) {
            continue;
        }
        if (&buffers != boundBuffers) {
            const SDL_GPUBufferBinding indices{buffers.indices, 0};
            SDL_BindGPUIndexBuffer(f.pass, &indices, SDL_GPU_INDEXELEMENTSIZE_32BIT);
            boundBuffers = &buffers;
        }
        const scene::Material* m = d.material;
        const bool skinned = d.primitive->skinned;
        const int kind = blend && m && m->alpha == scene::AlphaMode::Add ? 2 : blend;
        SDL_GPUGraphicsPipeline* pipeline = (skinned ? skinnedPipelines_ : pipelines_)[m && m->doubleSided ? 1 : 0][kind];
        if (pipeline != bound) {
            SDL_BindGPUGraphicsPipeline(f.pass, pipeline);
            bound = pipeline;
            boundSamplers = {};
            jointsBound = false;
        }
        if (skinned && !jointsBound) {
            SDL_BindGPUVertexStorageBuffers(f.pass, 0, &jointBuffer_, 1);
            jointsBound = true;
        }
        const DrawUniforms u = uniformsFor(f, d, blend);
        if (skinned) {
            SDL_PushGPUVertexUniformData(f.cmd, 0, &u.vertex, sizeof(u.vertex));
        } else {
            SDL_PushGPUVertexUniformData(f.cmd, 0, &u.vertex.frame, sizeof(u.vertex.frame));
        }
        SDL_PushGPUFragmentUniformData(f.cmd, 0, &u.fragment, sizeof(u.fragment));
        const auto same = [](const SDL_GPUTextureSamplerBinding& a, const SDL_GPUTextureSamplerBinding& b) {
            return a.texture == b.texture && a.sampler == b.sampler;
        };
        if (!std::equal(u.samplers.begin(), u.samplers.end(), boundSamplers.begin(), same)) {
            SDL_BindGPUFragmentSamplers(f.pass, 0, u.samplers.data(), static_cast<Uint32>(u.samplers.size()));
            boundSamplers = u.samplers;
        }
        // A skinned primitive's two vertex streams start where its own part of each buffer does. The
        // others share one binding, with the first vertex given to the draw.
        const auto& p = *d.primitive;
        if (skinned) {
            const SDL_GPUBufferBinding vertices[2] = {{buffers.vertices, p.firstVertex * kVertexFloats * static_cast<Uint32>(sizeof(float))},
                                                      {buffers.skin, p.firstSkin}};
            SDL_BindGPUVertexBuffers(f.pass, 0, vertices, 2);
        } else {
            const SDL_GPUBufferBinding vertices{buffers.vertices, 0};
            SDL_BindGPUVertexBuffers(f.pass, 0, &vertices, 1);
        }
        SDL_DrawGPUIndexedPrimitives(f.pass, p.count, 1, p.firstIndex, skinned ? 0 : static_cast<Sint32>(p.firstVertex), 0);
    }
}

// One draw's uniforms and textures: its place, the lights it is lit by, and its material.
Renderer::DrawUniforms Renderer::uniformsFor(const Frame& f, const Draw& d, int blend) const {
    DrawUniforms u;
    SkinnedUniforms& vu = u.vertex;
    std::copy(f.viewProjection.m.begin(), f.viewProjection.m.end(), vu.frame.viewProjection);
    const auto& t = d.instance->transform.m;
    const float model[16] = {t[0], t[1], t[2], t[3], t[4], t[5], t[6], t[7], t[8], t[9], t[10], t[11], 0, 0, 0, 1};
    std::copy(std::begin(model), std::end(model), vu.frame.model);
    std::copy(f.camera->eye.begin(), f.camera->eye.end(), vu.frame.eye);
    if (const scene::Material* lit = d.material; lit && lit->specPower > 0 && !lit->simLighting) {
        std::copy(lit->specCoef.begin(), lit->specCoef.end(), vu.frame.specular);
        vu.frame.specular[3] = lit->specPower;
    }
    const scene::Material* m = d.material;
    const bool skirt = d.instance->part == scene::Part::Skirt;
    const scene::ObjectLighting* lighting = objectLighting_ ? &*objectLighting_ : nullptr;
    const auto& parts = d.instance->partLightSets;
    const int set = d.primitive->source < parts.size() ? parts[d.primitive->source] : d.instance->lightSet;
    if (set >= 0 && static_cast<std::size_t>(set) < lightSets_.size()) {
        lighting = &lightSets_[static_cast<std::size_t>(set)];
    }
    const bool roof = d.instance->part == scene::Part::Roof && m && m->roofLight && objectLighting_;
    // A roof's underside, trim and edges: standard materials, lit as objects outdoors.
    const bool roofEdge = d.instance->part == scene::Part::Roof && m && !m->roofLight && !m->unlit && objectLighting_;
    // As the game's standard and Sim materials: on lit surfaces with tangents, the first lights per
    // pixel, through the normal map. Sims' even without one.
    SDL_GPUTexture* normalMap = bumpMapping_ && d.primitive->tangents && m && m->normalMap >= 0 &&
                                        static_cast<std::size_t>(m->normalMap) < gpu_->textures.size()
                                    ? gpu_->textures[static_cast<std::size_t>(m->normalMap)]
                                    : nullptr;
    const bool bumped = bumpMapping_ && d.primitive->tangents && m && !m->unlit && (normalMap != nullptr || m->simLighting);
    if (roof || roofEdge || (d.instance->litOutdoors && objectLighting_)) {
        lighting = &*objectLighting_;
    }
    if (lighting && (d.instance->part == scene::Part::Object || skirt || roof || roofEdge || d.instance->litOutdoors)) {
        setLights(u, d, *lighting, skirt, roof, bumped);
    }
    if (d.primitive->skinned) {
        vu.jointBase = static_cast<std::uint32_t>(jointBase_[static_cast<std::size_t>(d.instance - instances_.data())]);
    }
    setMaterial(u, f, d, blend, normalMap, lighting);
    return u;
}

// The ambient, thumbnail tint and lights of a lit object, skirt or roof.
void Renderer::setLights(DrawUniforms& u, const Draw& d, const scene::ObjectLighting& l, bool skirt, bool roof, bool bumped) const {
    SkinnedUniforms& vu = u.vertex;
    const scene::Material* m = d.material;
    for (std::size_t k = 0; k < 3; ++k) {
        // The skirt's and roofs' ambient: a quarter of it as a byte (the device's ambient),
        // times the material's ambient (the skirt's own, a roof's four times its diffuse).
        const float device = std::round(std::clamp(l.ambient[k] / 4, 0.0f, 1.0f) * 255) / 255;
        vu.frame.ambient[k] = skirt ? device * d.instance->skirtAmbient : roof ? device * 4 : l.ambient[k];
    }
    if (highlightObject_ != 0 && d.instance->part == scene::Part::Object && d.instance->object == highlightObject_) {
        for (std::size_t k = 0; k < 3; ++k) {
            vu.frame.ambient[k] += highlightAmbient_;
        }
    }
    vu.frame.ambient[3] = skirt ? 3.0f : 1.0f;
    if (skirt) {
        std::copy(d.instance->skirtScale.begin(), d.instance->skirtScale.end(), vu.frame.thumbnail);
    } else if (roof) {
        // The vertex colour clamped, then doubled by the texture stage: the same as a thumbnail.
        vu.frame.thumbnail[0] = m->color[0];
        vu.frame.thumbnail[1] = m->color[1];
        vu.frame.thumbnail[2] = m->color[2];
        vu.frame.thumbnail[3] = 0.5f;
    } else if (l.thumbnailScale > 0) {
        const auto c = m ? m->color : std::array<float, 4>{1, 1, 1, 1};
        vu.frame.thumbnail[0] = c[0];
        vu.frame.thumbnail[1] = c[1];
        vu.frame.thumbnail[2] = c[2];
        vu.frame.thumbnail[3] = l.thumbnailScale;
    }
    if (bumped && !skirt && !roof && l.thumbnailScale <= 0) {
        const auto chosen = scene::perPixelLights(l, kObjectLights);
        vu.frame.pixelLights[0] = static_cast<std::int32_t>(chosen.size());
        for (std::size_t k = 0; k < chosen.size(); ++k) {
            vu.frame.pixelLights[k + 1] = static_cast<std::int32_t>(chosen[k]);
        }
    }
    for (std::size_t i = 0; i < kObjectLights && i < l.lights.size(); ++i) {
        std::copy(l.lights[i].direction.begin(), l.lights[i].direction.end(), vu.frame.lightDirections[i]);
        if (l.lights[i].point) {
            vu.frame.lightDirections[i][3] = l.lights[i].range * l.lights[i].range;
            std::copy(l.lights[i].position.begin(), l.lights[i].position.end(), vu.frame.lightPositions[i]);
            vu.frame.lightPositions[i][3] = l.lights[i].falloff;
            if (l.lights[i].spot) {
                const float width = l.lights[i].coneInner - l.lights[i].coneOuter;
                vu.frame.lightCones[i][0] = l.lights[i].coneOuter;
                vu.frame.lightCones[i][1] = width > 1e-6f ? 1 / width : 1e6f;
                vu.frame.lightCones[i][2] = 1;
            }
        }
        const float share = l.lights[i].sun && !skirt ? d.sun : 1.0f;
        for (std::size_t k = 0; k < 3; ++k) {
            vu.frame.lightColors[i][k] = l.lights[i].color[k] * share;
        }
    }
}

// The material's color, texture and alpha test, the light map a floor or wall is lit by, and the
// textures to bind.
void Renderer::setMaterial(DrawUniforms& u, const Frame& f, const Draw& d, int blend, SDL_GPUTexture* normalMap,
                           const scene::ObjectLighting* lighting) const {
    const SkinnedUniforms& vu = u.vertex;
    FragmentUniforms& fu = u.fragment;
    const scene::Material* m = d.material;
    const auto c = m ? m->color : std::array<float, 4>{0.8f, 0.8f, 0.8f, 1};
    std::copy(c.begin(), c.end(), fu.color);
    if (night_ && m && m->nightDimmed) {
        fu.color[0] *= 0.35f;
        fu.color[1] *= 0.35f;
        fu.color[2] *= 0.7f;
    }
    fu.sunDirection[0] = f.sun[0];
    fu.sunDirection[1] = f.sun[1];
    fu.sunDirection[2] = f.sun[2];
    SDL_GPUTexture* texture = gpu_->white;
    if (m && m->image >= 0 && static_cast<std::size_t>(m->image) < gpu_->textures.size() && gpu_->textures[static_cast<std::size_t>(m->image)]) {
        texture = gpu_->textures[static_cast<std::size_t>(m->image)];
        fu.hasTexture = 1;
    }
    fu.alphaCutoff = !m                                     ? 0.0f
                     : m->alpha == scene::AlphaMode::Mask  ? m->alphaReference
                     : m->alpha == scene::AlphaMode::Layer ? 10.0f / 255  // as the game's terrain paint
                     : m->alpha == scene::AlphaMode::Add   ? 1.0f / 255   // as the game's particles
                     : blend                               ? 0.01f
                                                           : 0.0f;
    fu.unlit = m && m->unlit ? 1.0f : 0.0f;
    // A lot's floors and walls: lit by their light map alone.
    SDL_GPUTexture* lightMap = gpu_->white;
    SDL_GPUTexture* lightMapFrom = nullptr;
    SDL_GPUTexture* incidence = gpu_->white;
    fu.lightMapFade = 1;
    if (const int i = d.instance->lightMap; d.primitive->lightMapped && i >= 0 && static_cast<std::size_t>(i) < gpu_->textures.size() &&
                                            gpu_->textures[static_cast<std::size_t>(i)]) {
        lightMap = gpu_->textures[static_cast<std::size_t>(i)];
        if (const auto from = gpu_->fadingFrom.find(i); from != gpu_->fadingFrom.end() && from->second) {
            lightMapFrom = from->second;
            fu.lightMapFade = gpu_->lightMapFade;
        }
        fu.lightMap[0] = d.instance->lightMapGain;
        fu.lightMap[1] = d.instance->lightMapOnly ? 1.0f : 0.0f;
        fu.lightMap[2] = d.instance->lightMapMultiply ? 1.0f : 0.0f;
        // A floor's or wall's normal map, against its light map's incidence map.
        const int k = d.instance->incidenceMap;
        if (bumpMapping_ && floorAndWallNormalMapping_ && m && m->normalMap >= 0 && static_cast<std::size_t>(m->normalMap) < gpu_->textures.size() &&
            k >= 0 && static_cast<std::size_t>(k) < gpu_->textures.size() && gpu_->textures[static_cast<std::size_t>(m->normalMap)] &&
            gpu_->textures[static_cast<std::size_t>(k)] && !d.instance->lightMapMultiply) {
            normalMap = gpu_->textures[static_cast<std::size_t>(m->normalMap)];
            incidence = gpu_->textures[static_cast<std::size_t>(k)];
            fu.lightMap[3] = 1;
        }
    }
    fu.pixelLightCount = static_cast<float>(vu.frame.pixelLights[0]);
    fu.normalMapped = normalMap != nullptr ? 1.0f : 0.0f;
    fu.simLighting = m && m->simLighting && lighting && d.instance->part == scene::Part::Object && vu.frame.thumbnail[3] <= 0 ? 1.0f : 0.0f;
    for (std::int32_t k = 0; k < vu.frame.pixelLights[0]; ++k) {
        const auto& lightColor = vu.frame.lightColors[vu.frame.pixelLights[k + 1]];
        std::copy(lightColor, lightColor + 4, fu.pixelLightColors[k]);
    }
    if (m && (m->emissive[0] > 0 || m->emissive[1] > 0 || m->emissive[2] > 0)) {
        std::copy(m->emissive.begin(), m->emissive.end(), fu.emissive);
        fu.emissive[3] = 1;
    }
    // The environment cube, black for materials without one so the binding rarely changes.
    SDL_GPUTexture* cube = gpu_->blackCube;
    if (m && m->envCube >= 0 && static_cast<std::size_t>(m->envCube) < gpu_->envCubes.size() &&
        gpu_->envCubes[static_cast<std::size_t>(m->envCube)]) {
        cube = gpu_->envCubes[static_cast<std::size_t>(m->envCube)];
        std::copy(m->envCubeCoef.begin(), m->envCubeCoef.end(), fu.envCube);
        fu.envCube[3] = 1;
        if (m->envCubeLocked) {
            const Camera& camera = *f.camera;
            const Vec3 forward = normalize(sub(camera.target, camera.eye));
            // Looking straight down or up, the world's y stands in for up so right stays defined.
            const Vec3 side = cross(forward, {0, 0, 1});
            const Vec3 right = normalize(dot(side, side) > 1e-8f ? side : cross(forward, {0, 1, 0}));
            const Vec3 up = cross(right, forward);
            std::copy(right.begin(), right.end(), fu.cameraRight);
            std::copy(up.begin(), up.end(), fu.cameraUp);
            std::copy(forward.begin(), forward.end(), fu.cameraForward);
            fu.cameraRight[3] = 1;
        }
    }
    u.samplers = {{{texture, m && m->clamp ? clampedSampler_ : sampler_},
                   {lightMap, clampedSampler_},
                   {normalMap ? normalMap : gpu_->white, sampler_},
                   {incidence, clampedSampler_},
                   {lightMapFrom ? lightMapFrom : lightMap, clampedSampler_},
                   {cube, clampedSampler_}}};
}

void Renderer::drawRoads(const Frame& f) {
    SDL_BindGPUGraphicsPipeline(f.pass, roadPipeline_);
    SDL_PushGPUVertexUniformData(f.cmd, 0, f.viewProjection.m.data(), 16 * sizeof(float));
    const float road[4] = {gpu_->lightMapSize, 0, 0, 0};
    SDL_PushGPUFragmentUniformData(f.cmd, 0, road, sizeof(road));
    SDL_GPUTextureSamplerBinding bindings[2] = {{gpu_->roadTextures, clampedMips_}, {gpu_->terrainLight, sampler_}};
    SDL_BindGPUFragmentSamplers(f.pass, 0, bindings, 2);
    SDL_GPUBufferBinding vertices{gpu_->roadVertices, 0};
    SDL_BindGPUVertexBuffers(f.pass, 0, &vertices, 1);
    SDL_GPUBufferBinding indices{gpu_->roadIndices, 0};
    SDL_BindGPUIndexBuffer(f.pass, &indices, SDL_GPU_INDEXELEMENTSIZE_32BIT);
    SDL_DrawGPUIndexedPrimitives(f.pass, gpu_->roadIndexCount, 1, 0, 0, 0);
}

void Renderer::drawWater(const Frame& f) {
    SDL_BindGPUGraphicsPipeline(f.pass, waterPipeline_);
    SDL_PushGPUVertexUniformData(f.cmd, 0, f.viewProjection.m.data(), 16 * sizeof(float));
    WaterUniforms w = gpu_->waterUniforms;
    w.screen[0] = static_cast<float>(f.width);
    w.screen[1] = static_cast<float>(f.height);
    w.time = time_;
    SDL_PushGPUFragmentUniformData(f.cmd, 0, &w, sizeof(w));
    SDL_GPUTextureSamplerBinding bindings[2] = {{reflection_, spriteSampler_}, {gpu_->ripples, sampler_}};
    SDL_BindGPUFragmentSamplers(f.pass, 0, bindings, 2);
    SDL_GPUBufferBinding vertices{gpu_->waterVertices, 0};
    SDL_BindGPUVertexBuffers(f.pass, 0, &vertices, 1);
    SDL_GPUBufferBinding indices{gpu_->waterIndices, 0};
    SDL_BindGPUIndexBuffer(f.pass, &indices, SDL_GPU_INDEXELEMENTSIZE_32BIT);
    SDL_DrawGPUIndexedPrimitives(f.pass, gpu_->waterIndexCount, 1, 0, 0, 0);
}

// Sprites in order, one draw per run of the same texture.
void Renderer::drawSprites(const Frame& f) {
    SDL_BindGPUGraphicsPipeline(f.pass, spritePipeline_);
    const float screen[4] = {static_cast<float>(f.width), static_cast<float>(f.height), 0, 0};
    SDL_PushGPUVertexUniformData(f.cmd, 0, screen, sizeof(screen));
    SDL_GPUBufferBinding vertices{spriteVertices_, 0};
    SDL_BindGPUVertexBuffers(f.pass, 0, &vertices, 1);
    for (std::size_t i = 0; i < sprites_.size();) {
        const int t = sprites_[i].texture;
        std::size_t j = i;
        while (j < sprites_.size() && sprites_[j].texture == t) {
            ++j;
        }
        const bool has = t >= 0 && static_cast<std::size_t>(t) < spriteTextures_.size() && spriteTextures_[static_cast<std::size_t>(t)];
        if (!has && !gpu_->white) {
            i = j;
            continue;
        }
        SDL_GPUTextureSamplerBinding binding{has ? spriteTextures_[static_cast<std::size_t>(t)] : gpu_->white, spriteSampler_};
        SDL_BindGPUFragmentSamplers(f.pass, 0, &binding, 1);
        SDL_DrawGPUPrimitives(f.pass, static_cast<Uint32>((j - i) * 6), 1, static_cast<Uint32>(i * 6), 0);
        i = j;
    }
}

} // namespace ts2::render
