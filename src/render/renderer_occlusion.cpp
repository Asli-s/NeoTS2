// Ambient occlusion, not in the original game: the Graphics panel's setting darkens corners and contacts. The
// opaque surfaces are drawn into a depth texture of their own, as the main view's may be multisampled and
// can't be read. How open each pixel is comes from that, and the main view's opaque surfaces and layers
// are multiplied by it as they are drawn, so glass, water and particles aren't darkened.

#include "render/renderer.hpp"
#include "render/renderer_internals.hpp"
#include "diag/log.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <exception>
#include <string>
#include <tuple>

namespace ts2::render {

using namespace render_detail;

namespace {

// How far around a point counts, in tiles.
constexpr float kRadius = 0.6f;

// What ao.frag is given, field for field.
struct OcclusionUniforms {
    float clipToWorld[16];
    float worldToClip[16];
    float eye[4];
    float size[4];
    float radius;
    float strength;
    float unused[2];
};
static_assert(sizeof(OcclusionUniforms) % 16 == 0);

} // namespace

void SdlGpuRenderer::releaseOcclusion() {
    for (auto* rows : {occlusion_.depthPipelines, occlusion_.solidPipelines}) {
        for (int skinned = 0; skinned < 2; ++skinned) {
            for (auto* p : rows[skinned]) {
                if (p) {
                    SDL_ReleaseGPUGraphicsPipeline(device_, p);
                }
            }
        }
    }
    for (auto* p : {occlusion_.occlusionPipeline, occlusion_.blurPipeline, occlusion_.upsamplePipeline, occlusion_.halfDepthPipeline}) {
        if (p) {
            SDL_ReleaseGPUGraphicsPipeline(device_, p);
        }
    }
    for (auto* t : {occlusion_.depth, occlusion_.halfDepth, occlusion_.raw, occlusion_.soft, occlusion_.blurred}) {
        if (t) {
            SDL_ReleaseGPUTexture(device_, t);
        }
    }
    occlusion_ = {};
}

void SdlGpuRenderer::setAmbientOcclusion(float strength) {
    occlusionStrength_ = strength;
    const bool on = strength > 0;
    if (on == (occlusion_.occlusionPipeline != nullptr)) {
        return;
    }
    releaseOcclusion();
    if (on) {
        try {
            makeOcclusionPipelines();
        } catch (const std::exception& error) {
            diag::warnOnce(std::string("ambient occlusion: ") + error.what());
            releaseOcclusion();
        }
    }
}

void SdlGpuRenderer::setAmbientOcclusionHalfSize(bool half) {
    occlusionHalf_ = half;
}

bool SdlGpuRenderer::occlusionReady(unsigned width, unsigned height) {
    if (occlusion_.occlusionPipeline == nullptr) {
        return false;
    }
    const bool halfMissing = occlusionHalf_ && (occlusion_.halfDepth == nullptr || occlusion_.soft == nullptr);
    if (occlusion_.depth == nullptr || occlusion_.raw == nullptr || halfMissing || occlusion_.blurred == nullptr || occlusion_.width != width ||
        occlusion_.height != height || occlusion_.half != occlusionHalf_) {
        for (auto** t : {&occlusion_.depth, &occlusion_.halfDepth, &occlusion_.raw, &occlusion_.soft, &occlusion_.blurred}) {
            if (*t) {
                SDL_ReleaseGPUTexture(device_, *t);
                *t = nullptr;
            }
        }
        SDL_GPUTextureCreateInfo info{};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
        info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        info.width = width;
        info.height = height;
        info.layer_count_or_depth = 1;
        info.num_levels = 1;
        occlusion_.depth = SDL_CreateGPUTexture(device_, &info);
        info.format = kOcclusionFormat;
        info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        occlusion_.blurred = SDL_CreateGPUTexture(device_, &info);
        const unsigned scale = occlusionHalf_ ? 2 : 1;
        info.width = (width + scale - 1) / scale;
        info.height = (height + scale - 1) / scale;
        occlusion_.raw = SDL_CreateGPUTexture(device_, &info);
        occlusion_.soft = occlusionHalf_ ? SDL_CreateGPUTexture(device_, &info) : nullptr;
        info.format = kHalfDepthFormat;
        occlusion_.halfDepth = occlusionHalf_ ? SDL_CreateGPUTexture(device_, &info) : nullptr;
        occlusion_.width = width;
        occlusion_.height = height;
        occlusion_.half = occlusionHalf_;
        if (occlusion_.depth == nullptr || occlusion_.raw == nullptr || (occlusionHalf_ && (occlusion_.halfDepth == nullptr || occlusion_.soft == nullptr)) ||
            occlusion_.blurred == nullptr) {
            diag::warnOnce("ambient occlusion: no video memory for its targets, so it is left out");
            occlusion_.width = occlusion_.height = 0;
            return false;
        }
    }
    return true;
}

// `uniforms` are the main pass's, one for each of `opaque`.
void SdlGpuRenderer::drawOcclusion(const Frame& f, std::span<const Draw> opaque, std::span<const DrawUniforms> uniforms) {
    SDL_GPUDepthStencilTargetInfo depth{};
    depth.texture = occlusion_.depth;
    depth.clear_depth = 0;
    depth.load_op = SDL_GPU_LOADOP_CLEAR;
    depth.store_op = SDL_GPU_STOREOP_STORE;
    depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(f.cmd, nullptr, 0, &depth);
    SDL_GPUGraphicsPipeline* bound = nullptr;
    const MeshBuffers* boundBuffers = nullptr;
    for (std::size_t i = 0; i < opaque.size(); ++i) {
        const Draw& d = opaque[i];
        const MeshBuffers& buffers = gpu_->meshBuffers[d.primitive->buffers];
        if (buffers.vertices == nullptr || buffers.indices == nullptr) {
            continue;
        }
        if (&buffers != boundBuffers) {
            const SDL_GPUBufferBinding indices{buffers.indices, 0};
            SDL_BindGPUIndexBuffer(pass, &indices, SDL_GPU_INDEXELEMENTSIZE_32BIT);
            boundBuffers = &buffers;
        }
        const bool skinned = d.primitive->skinned;
        const DrawUniforms& u = uniforms[i];
        // The same tests as ao_depth.frag's: a surface that can't leave a hole needs no fragment shader work.
        const bool holes = u.fragment.wallMask[0] > 0.5f || u.fragment.alphaCutoff > 0;
        SDL_GPUGraphicsPipeline* pipeline = (holes ? occlusion_.depthPipelines : occlusion_.solidPipelines)[skinned ? 1 : 0][d.material && d.material->doubleSided ? 1 : 0];
        if (pipeline != bound) {
            SDL_BindGPUGraphicsPipeline(pass, pipeline);
            bound = pipeline;
            if (skinned) {
                SDL_BindGPUVertexStorageBuffers(pass, 0, &jointBuffer_, 1);
            }
        }
        if (skinned) {
            SDL_PushGPUVertexUniformData(f.cmd, 0, &u.vertex, sizeof(u.vertex));
        } else {
            SDL_PushGPUVertexUniformData(f.cmd, 0, &u.vertex.frame, sizeof(u.vertex.frame));
        }
        if (holes) {
            SDL_PushGPUFragmentUniformData(f.cmd, 0, &u.fragment, sizeof(u.fragment));
            const SDL_GPUTextureSamplerBinding cut[2] = {{textureFor(u.samplers[0].texture), samplerFor(u.samplers[0].sampler)},
                                                         {textureFor(u.samplers[6].texture), samplerFor(u.samplers[6].sampler)}};
            SDL_BindGPUFragmentSamplers(pass, 0, cut, 2);
        }
        const auto& p = *d.primitive;
        if (skinned) {
            const SDL_GPUBufferBinding vertices[2] = {{buffers.vertices, p.firstVertex * kVertexFloats * static_cast<Uint32>(sizeof(float))}, {buffers.skin, p.firstSkin}};
            SDL_BindGPUVertexBuffers(pass, 0, vertices, 2);
            bindSkinStorage(pass, buffers);
        } else {
            const SDL_GPUBufferBinding vertices{buffers.vertices, 0};
            SDL_BindGPUVertexBuffers(pass, 0, &vertices, 1);
        }
        SDL_DrawGPUIndexedPrimitives(pass, p.count, 1, p.firstIndex, skinned ? 0 : static_cast<Sint32>(p.firstVertex), 0);
    }
    SDL_EndGPURenderPass(pass);

    OcclusionUniforms u{};
    const Mat4 toWorld = f.viewProjection.inverse();
    std::copy(toWorld.m.begin(), toWorld.m.end(), u.clipToWorld);
    std::copy(f.viewProjection.m.begin(), f.viewProjection.m.end(), u.worldToClip);
    u.eye[0] = f.camera->eye[0];
    u.eye[1] = f.camera->eye[1];
    u.eye[2] = f.camera->eye[2];
    const unsigned scale = occlusion_.half ? 2 : 1;
    u.size[0] = static_cast<float>((occlusion_.width + scale - 1) / scale);
    u.size[1] = static_cast<float>((occlusion_.height + scale - 1) / scale);
    u.size[2] = static_cast<float>(occlusion_.width);
    u.size[3] = static_cast<float>(occlusion_.height);
    u.radius = kRadius;
    u.strength = occlusionStrength_;
    // At half size the blur reads the depths the occlusion worked on.
    SDL_GPUTexture* depthRead = occlusion_.half ? occlusion_.halfDepth : occlusion_.depth;
    if (occlusion_.half) {
        SDL_GPUColorTargetInfo halfDepth{};
        halfDepth.texture = occlusion_.halfDepth;
        halfDepth.load_op = SDL_GPU_LOADOP_DONT_CARE;
        halfDepth.store_op = SDL_GPU_STOREOP_STORE;
        pass = SDL_BeginGPURenderPass(f.cmd, &halfDepth, 1, nullptr);
        SDL_BindGPUGraphicsPipeline(pass, occlusion_.halfDepthPipeline);
        const SDL_GPUTextureSamplerBinding full{occlusion_.depth, shadowPoint_};
        SDL_BindGPUFragmentSamplers(pass, 0, &full, 1);
        SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
        SDL_EndGPURenderPass(pass);
    }
    SDL_GPUColorTargetInfo raw{};
    raw.texture = occlusion_.raw;
    raw.load_op = SDL_GPU_LOADOP_DONT_CARE;
    raw.store_op = SDL_GPU_STOREOP_STORE;
    pass = SDL_BeginGPURenderPass(f.cmd, &raw, 1, nullptr);
    SDL_BindGPUGraphicsPipeline(pass, occlusion_.occlusionPipeline);
    SDL_PushGPUFragmentUniformData(f.cmd, 0, &u, sizeof(u));
    const SDL_GPUTextureSamplerBinding depthOnly{occlusion_.depth, shadowPoint_};
    SDL_BindGPUFragmentSamplers(pass, 0, &depthOnly, 1);
    SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
    SDL_EndGPURenderPass(pass);

    // The blur, then at half size the result brought up to the view's size.
    const std::tuple<SDL_GPUTexture*, SDL_GPUGraphicsPipeline*, std::array<SDL_GPUTexture*, 3>> steps[2] = {
        {occlusion_.half ? occlusion_.soft : occlusion_.blurred, occlusion_.blurPipeline, {occlusion_.raw, depthRead, nullptr}},
        {occlusion_.blurred, occlusion_.upsamplePipeline, {occlusion_.soft, occlusion_.halfDepth, occlusion_.depth}}};
    for (int i = 0; i < (occlusion_.half ? 2 : 1); ++i) {
        const auto& [to, pipeline, from] = steps[i];
        SDL_GPUColorTargetInfo target{};
        target.texture = to;
        target.load_op = SDL_GPU_LOADOP_DONT_CARE;
        target.store_op = SDL_GPU_STOREOP_STORE;
        pass = SDL_BeginGPURenderPass(f.cmd, &target, 1, nullptr);
        SDL_BindGPUGraphicsPipeline(pass, pipeline);
        SDL_GPUTextureSamplerBinding samplers[3]{};
        const Uint32 count = from[2] != nullptr ? 3 : 2;
        for (Uint32 k = 0; k < count; ++k) {
            samplers[k] = {from[k], shadowPoint_};
        }
        SDL_BindGPUFragmentSamplers(pass, 0, samplers, count);
        SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
        SDL_EndGPURenderPass(pass);
    }
}

} // namespace ts2::render
