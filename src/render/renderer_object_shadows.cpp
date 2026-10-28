// The lot objects' shadows from the sun (see scene::objectshadow): their pictures drawn into the
// atlas, and the places' list and grid the scene's fragment shader reads.

#include "render/renderer.hpp"
#include "render/renderer_internals.hpp"

#include "diag/log.hpp"
#include "scene/sim_shadows.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace ts2::render {

using namespace render_detail;

namespace {

constexpr float kSide = scene::simshadows::kResolution;
constexpr float kAtlas = kSide * kObjectTilesAcross;

} // namespace

void SdlGpuRenderer::updateObjectShadows(const Frame& f) {
    if (shadows_.objects == objectLayout_) {
        return;
    }
    objectGridArea_ = {};
    // The pictures wait for their models to reach the GPU (the scene's meshes upload in the background).
    if (shadows_.objects) {
        for (const auto& picture : shadows_.objects->pictures) {
            if (picture.mesh >= 0 && static_cast<std::size_t>(picture.mesh) >= meshes().size()) {
                return;
            }
        }
    }
    if (shadows_.objects && !uploadObjectPlaces(f, *shadows_.objects)) {
        return;
    }
    objectLayout_ = shadows_.objects;
    if (objectLayout_) {
        drawObjectPictures(f, *objectLayout_);
    }
}

// Each picture: the model drawn flat into the scratch picture, then blurred into its tile.
void SdlGpuRenderer::drawObjectPictures(const Frame& f, const scene::objectshadow::Layout& layout) {
    const auto& gpuMeshes = meshes();
    ObjectBlurUniforms blur{};
    const ShadowBlurUniforms weights = FramePlan::shadowBlurUniforms();
    std::copy(&weights.weights[0][0], &weights.weights[0][0] + 25 * 4, &blur.weights[0][0]);
    const std::size_t count = std::min(layout.pictures.size(), kMaxObjectPictures);
    for (std::size_t i = 0; i < count; ++i) {
        const auto& picture = layout.pictures[i];
        SDL_GPUColorTargetInfo scratch{};
        scratch.texture = objectScratch_;
        scratch.clear_color = SDL_FColor{1, 1, 1, 0};
        scratch.load_op = SDL_GPU_LOADOP_CLEAR;
        scratch.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(f.cmd, &scratch, 1, nullptr);
        if (picture.mesh >= 0 && static_cast<std::size_t>(picture.mesh) < gpuMeshes.size()) {
            const SDL_GPUViewport whole{0, 0, kSide, kSide, 0, 1};
            SDL_SetGPUViewport(pass, &whole);
            SDL_BindGPUGraphicsPipeline(pass, objectCasterPipeline_);
            VertexUniforms u{};
            std::copy(picture.camera.begin(), picture.camera.end(), u.viewProjection);
            const double a = picture.facing * 3.14159265358979323846 / 4;
            const float c = static_cast<float>(std::cos(a)), s = static_cast<float>(std::sin(a));
            const float turn[16] = {c, -s, 0, 0, s, c, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            std::copy(std::begin(turn), std::end(turn), u.model);
            SDL_PushGPUVertexUniformData(f.cmd, 0, &u, sizeof(u));
            for (const auto& p : gpuMeshes[static_cast<std::size_t>(picture.mesh)].primitives) {
                // The game switches the shadow groups off in the model it draws.
                if (p.skinned || p.shadowGroup || p.buffers >= gpu_->meshBuffers.size()) {
                    continue;
                }
                const MeshBuffers& buffers = gpu_->meshBuffers[p.buffers];
                if (buffers.vertices == nullptr || buffers.indices == nullptr) {
                    continue;
                }
                const SDL_GPUBufferBinding indices{buffers.indices, 0};
                SDL_BindGPUIndexBuffer(pass, &indices, SDL_GPU_INDEXELEMENTSIZE_32BIT);
                const SDL_GPUBufferBinding vertices{buffers.vertices, 0};
                SDL_BindGPUVertexBuffers(pass, 0, &vertices, 1);
                SDL_DrawGPUIndexedPrimitives(pass, p.count, 1, p.firstIndex, static_cast<Sint32>(p.firstVertex), 0);
            }
        }
        SDL_EndGPURenderPass(pass);

        SDL_GPUColorTargetInfo atlas{};
        atlas.texture = objectPictures_;
        atlas.clear_color = SDL_FColor{0, 0, 0, 0};
        atlas.load_op = i == 0 ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
        atlas.store_op = SDL_GPU_STOREOP_STORE;
        pass = SDL_BeginGPURenderPass(f.cmd, &atlas, 1, nullptr);
        const float x = static_cast<float>(i % kObjectTilesAcross) * kSide, y = static_cast<float>(i / kObjectTilesAcross) * kSide;
        const float size = static_cast<float>(picture.size);
        const SDL_GPUViewport tile{x, y, size, size, 0, 1};
        SDL_SetGPUViewport(pass, &tile);
        SDL_BindGPUGraphicsPipeline(pass, objectBlurPipeline_);
        blur.tile[0] = x;
        blur.tile[1] = y;
        blur.tile[2] = size;
        SDL_PushGPUFragmentUniformData(f.cmd, 0, &blur, sizeof(blur));
        const SDL_GPUTextureSamplerBinding source{objectScratch_, shadowSampler_};
        SDL_BindGPUFragmentSamplers(pass, 0, &source, 1);
        SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
        SDL_EndGPURenderPass(pass);
    }
    if (layout.pictures.size() > kMaxObjectPictures) {
        diag::warnOnce("object shadows: pictures past 256 are not drawn");
    }
}

// The places' list (a row each) and the grid over the tiles they reach.
bool SdlGpuRenderer::uploadObjectPlaces(const Frame& f, const scene::objectshadow::Layout& layout) {
    std::vector<float> places;
    std::vector<std::array<float, 4>> rects;
    for (const auto& place : layout.places) {
        if (place.picture >= std::min(layout.pictures.size(), kMaxObjectPictures) || rects.size() >= kMaxObjectPlaces) {
            continue;
        }
        const auto& picture = layout.pictures[place.picture];
        places.insert(places.end(), place.camera.begin(), place.camera.begin() + 12);
        places.push_back(static_cast<float>(place.picture % kObjectTilesAcross) * kSide / kAtlas);
        places.push_back(static_cast<float>(place.picture / kObjectTilesAcross) * kSide / kAtlas);
        places.push_back(static_cast<float>(picture.size) / kAtlas);
        places.push_back(static_cast<float>(place.level));
        places.insert(places.end(), place.rect.begin(), place.rect.end());
        rects.push_back(place.rect);
    }
    if (rects.empty()) {
        return true;
    }
    float low[2] = {rects[0][0], rects[0][2]}, high[2] = {rects[0][1], rects[0][3]};
    for (const auto& r : rects) {
        low[0] = std::min(low[0], r[0]);
        high[0] = std::max(high[0], r[1]);
        low[1] = std::min(low[1], r[2]);
        high[1] = std::max(high[1], r[3]);
    }
    if (!std::isfinite(low[0]) || !std::isfinite(low[1]) || !std::isfinite(high[0]) || !std::isfinite(high[1])) {
        return true;
    }
    const int x0 = static_cast<int>(std::floor(low[0])), y0 = static_cast<int>(std::floor(low[1]));
    const int width = std::clamp(static_cast<int>(std::floor(high[0])) - x0 + 1, 1, kMaxObjectGrid);
    const int height = std::clamp(static_cast<int>(std::floor(high[1])) - y0 + 1, 1, kMaxObjectGrid);
    std::vector<float> grid(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 2 * 4, -1.0f);
    std::vector<int> filled(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0);
    for (std::size_t i = 0; i < rects.size(); ++i) {
        const auto& r = rects[i];
        const int ax = std::max(static_cast<int>(std::floor(r[0])) - x0, 0), bx = std::min(static_cast<int>(std::floor(r[1])) - x0, width - 1);
        const int ay = std::max(static_cast<int>(std::floor(r[2])) - y0, 0), by = std::min(static_cast<int>(std::floor(r[3])) - y0, height - 1);
        for (int ty = ay; ty <= by; ++ty) {
            for (int tx = ax; tx <= bx; ++tx) {
                const std::size_t t = static_cast<std::size_t>(ty) * static_cast<std::size_t>(width) + static_cast<std::size_t>(tx);
                if (filled[t] >= kObjectGridSlots) {
                    diag::warnOnce("object shadows: more than 8 objects reaching one tile, the rest cast none there");
                    continue;
                }
                grid[t * 8 + static_cast<std::size_t>(filled[t]++)] = static_cast<float>(i);
            }
        }
    }
    UploadPass pass(device_, f.cmd);
    auto upload = [&](SDL_GPUTexture* texture, const std::vector<float>& data, Uint32 w, Uint32 h) {
        const SDL_GPUTransferBufferLocation at = pass.stage(data.data(), static_cast<Uint32>(data.size() * sizeof(float)));
        if (at.transfer_buffer == nullptr) {
            return false;
        }
        SDL_GPUTextureTransferInfo from{};
        from.transfer_buffer = at.transfer_buffer;
        from.offset = at.offset;
        from.pixels_per_row = w;
        from.rows_per_layer = h;
        SDL_GPUTextureRegion to{};
        to.texture = texture;
        to.w = w;
        to.h = h;
        to.d = 1;
        SDL_UploadToGPUTexture(pass.copy(), &from, &to, false);
        return true;
    };
    const bool done = upload(objectPlaces_, places, kObjectPlaceTexels, static_cast<Uint32>(rects.size())) &&
                      upload(objectGrid_, grid, static_cast<Uint32>(2 * width), static_cast<Uint32>(height));
    pass.end();
    if (done) {
        objectGridArea_ = {static_cast<float>(x0), static_cast<float>(y0), static_cast<float>(width), static_cast<float>(height)};
    }
    return done;
}

} // namespace ts2::render
