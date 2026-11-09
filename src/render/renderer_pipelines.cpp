#include "render/renderer.hpp"
#include "render/renderer_internals.hpp"
#include "diag/log.hpp"
#include "scene/sim_shadows.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <exception>
#include <fstream>
#include <string>
#include <stdexcept>

namespace ts2::render {

using namespace render_detail;

namespace {

std::vector<std::uint8_t> readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("can't read " + diag::utf8(path));
    }
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// A compiled shader from the shader folder, named without its extension. Throws when it can't be read or made.
SDL_GPUShader* loadShader(SDL_GPUDevice* device, const std::filesystem::path& folder, const char* file, SDL_GPUShaderStage stage,
                          Uint32 samplers, Uint32 uniforms = 1, Uint32 storageBuffers = 0) {
    const auto code = readFile(folder / (std::string(file) + kShaderExtension));
    SDL_GPUShaderCreateInfo info{};
    info.code_size = code.size();
    info.code = code.data();
    info.entrypoint = kShaderEntrypoint;
    info.format = kShaderFormat;
    info.stage = stage;
    info.num_samplers = samplers;
    info.num_uniform_buffers = uniforms;
    info.num_storage_buffers = storageBuffers;
    SDL_GPUShader* s = SDL_CreateGPUShader(device, &info);
    if (s == nullptr) {
        throw sdlError(file);
    }
    return s;
}

// The scene's vertex layout: positions, normals and the rest in one buffer, and a skinned
// primitive's joints, weights, place in the morph data and eye coordinates in a second.
void sceneVertexLayout(SDL_GPUVertexBufferDescription (&buffers)[2], SDL_GPUVertexAttribute (&attributes)[11]) {
    buffers[0].slot = 0;
    buffers[0].pitch = kVertexFloats * sizeof(float);
    buffers[0].input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    buffers[1].slot = 1;
    buffers[1].pitch = kSkinBytes;
    buffers[1].input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    attributes[0] = {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 0};
    attributes[1] = {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 3 * sizeof(float)};
    attributes[2] = {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 6 * sizeof(float)};
    attributes[3] = {3, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT, 8 * sizeof(float)};
    attributes[4] = {6, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 9 * sizeof(float)};
    attributes[5] = {7, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 11 * sizeof(float)};
    attributes[6] = {8, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 14 * sizeof(float)};
    attributes[7] = {4, 1, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4, 0};
    attributes[8] = {5, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 4};
    attributes[9] = {9, 1, SDL_GPU_VERTEXELEMENTFORMAT_UINT, 4 + 4 * sizeof(float)};
    attributes[10] = {10, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, kSkinEyeOffset};
}

SDL_GPUSampleCount sampleCount(int samples) {
    return samples >= 8 ? SDL_GPU_SAMPLECOUNT_8 : samples >= 4 ? SDL_GPU_SAMPLECOUNT_4 : samples >= 2 ? SDL_GPU_SAMPLECOUNT_2 : SDL_GPU_SAMPLECOUNT_1;
}

} // namespace

void SdlGpuRenderer::create(const std::filesystem::path& shaderFolder) {
    SDL_GPUBufferCreateInfo none{};
    none.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
    none.size = 16;
    noMorphs_ = SDL_CreateGPUBuffer(device_, &none);
    if (noMorphs_ == nullptr) {
        throw sdlError("GPU buffer");
    }
    shaderFolder_ = shaderFolder;
    makeSamplers();
    makePassPipelines(single_, 1);
    makeScenePipelines(shaderFolder_, picture_, 1, true);
    makePoolPipeline(shaderFolder_, picture_, 1, true);
    makeCausticsPipelines(shaderFolder);
    makeShadowPipelines(shaderFolder);
    SDL_GPUTextureCreateInfo blank{};
    blank.type = SDL_GPU_TEXTURETYPE_2D;
    blank.format = kOcclusionFormat;
    blank.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    blank.width = blank.height = 1;
    blank.layer_count_or_depth = 1;
    blank.num_levels = 1;
    noOcclusion_ = SDL_CreateGPUTexture(device_, &blank);
    if (noOcclusion_ == nullptr) {
        throw sdlError("GPU texture");
    }
}

void SdlGpuRenderer::makePassPipelines(PassPipelines& out, int samples) {
    const std::filesystem::path& shaders = shaderFolder_;
    makeScenePipelines(shaders, out, samples);
    makeSpritePipeline(shaders, out, samples);
    makeParticlePipelines(shaders, out, samples);
    makeTerrainPipeline(shaders, out, samples);
    makeRoadPipeline(shaders, out, samples);
    makeImposterPipelines(shaders, out, samples);
    makeClipPipeline(shaders, out, samples);
    makeSkyPipeline(shaders, out, samples);
    makeWaterPipeline(shaders, out, samples);
    makePoolPipeline(shaders, out, samples);
    makeSeaPipeline(shaders, out, samples);
    makePondPipeline(shaders, out, samples);
}

void SdlGpuRenderer::releasePassPipelines(PassPipelines& pipelines) {
    const auto release = [&](SDL_GPUGraphicsPipeline* p) {
        if (p) {
            SDL_ReleaseGPUGraphicsPipeline(device_, p);
        }
    };
    for (auto* set : {&pipelines.scene, &pipelines.skinnedScene}) {
        for (auto& row : *set) {
            std::for_each(std::begin(row), std::end(row), release);
        }
    }
    std::for_each(pipelines.particles.begin(), pipelines.particles.end(), release);
    for (auto* p : {pipelines.sprite, pipelines.terrain, pipelines.road, pipelines.imposterLight, pipelines.imposterTerrain, pipelines.imposterWall, pipelines.imposterRoof,
                    pipelines.imposterFadedWall, pipelines.imposterFadedRoof, pipelines.clip, pipelines.sky, pipelines.water, pipelines.pond, pipelines.pool, pipelines.sea}) {
        release(p);
    }
    pipelines = {};
}

int SdlGpuRenderer::mostSmoothEdgeSamples() const {
    const SDL_GPUTextureFormat color = SDL_GetGPUSwapchainTextureFormat(device_, window_);
    int most = 1;
    for (const int samples : {2, 4, 8}) {
        if (SDL_GPUTextureSupportsSampleCount(device_, color, sampleCount(samples)) && SDL_GPUTextureSupportsSampleCount(device_, SDL_GPU_TEXTUREFORMAT_D32_FLOAT, sampleCount(samples))) {
            most = samples;
        }
    }
    return most;
}

void SdlGpuRenderer::setSmoothEdges(int samples) {
    // A card with fewer samples than a saved choice takes what it has.
    const int most = std::min(samples, mostSmoothEdgeSamples());
    const int wanted = most >= 8 ? 8 : most >= 4 ? 4 : most >= 2 ? 2 : 1;
    if (wanted == smoothSamples_) {
        return;
    }
    releasePassPipelines(smooth_);
    for (DepthBuffer* buffer : {&smoothColor_, &smoothDepth_}) {
        if (buffer->texture) {
            SDL_ReleaseGPUTexture(device_, buffer->texture);
        }
        *buffer = {};
    }
    smoothSamples_ = 1;
    if (wanted == 1) {
        return;
    }
    try {
        makePassPipelines(smooth_, wanted);
        smoothSamples_ = wanted;
    } catch (const std::exception& error) {
        diag::warnOnce(std::string("smooth edges: ") + error.what());
        releasePassPipelines(smooth_);
    }
}

// The scene's pipelines, by skinned, two-sided and blending, and again clipping depth for the pools'
// reflection (see poolMirror).
void SdlGpuRenderer::makeScenePipelines(const std::filesystem::path& shaders, PassPipelines& out, int samples, bool gameAlpha) {
    SDL_GPUShader* vertex = loadShader(device_, shaders, "scene.vert", SDL_GPU_SHADERSTAGE_VERTEX, 0);
    SDL_GPUShader* skinnedVertex = loadShader(device_, shaders, "skinned.vert", SDL_GPU_SHADERSTAGE_VERTEX, 0, 1, 2);
    SDL_GPUShader* fragment = loadShader(device_, shaders, "scene.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 16, 2);

    // Only a mirror image clips depth, and those are always drawn with one sample.
    for (int clip = 0; clip < (samples > 1 || gameAlpha ? 1 : 2); ++clip)
    for (int skinned = 0; skinned < 2; ++skinned)
    for (int twoSided = 0; twoSided < 2; ++twoSided) {
        for (int blend = 0; blend < (clip == 0 && samples <= 1 && !gameAlpha ? kOpaqueOverDepth + 1 : kOpaqueOverDepth); ++blend) {
            SDL_GPUVertexBufferDescription buffers[2]{};
            SDL_GPUVertexAttribute attributes[11]{};
            sceneVertexLayout(buffers, attributes);

            SDL_GPUColorTargetDescription color{};
            color.format = SDL_GetGPUSwapchainTextureFormat(device_, window_);
            if (blend && blend != kOpaqueOverDepth) {
                color.blend_state.enable_blend = true;
                const bool adds = blend == 2 || blend == 3;
                color.blend_state.src_color_blendfactor = blend == 3 ? SDL_GPU_BLENDFACTOR_ONE : SDL_GPU_BLENDFACTOR_SRC_ALPHA;
                color.blend_state.dst_color_blendfactor = adds ? SDL_GPU_BLENDFACTOR_ONE : SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
                color.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
                color.blend_state.src_alpha_blendfactor = adds ? SDL_GPU_BLENDFACTOR_ZERO : SDL_GPU_BLENDFACTOR_ONE;
                color.blend_state.dst_alpha_blendfactor = adds ? SDL_GPU_BLENDFACTOR_ONE : SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
                color.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
                if (gameAlpha) {
                    color.blend_state.src_alpha_blendfactor = color.blend_state.src_color_blendfactor;
                    color.blend_state.dst_alpha_blendfactor = color.blend_state.dst_color_blendfactor;
                }
            }

            SDL_GPUGraphicsPipelineCreateInfo info{};
            info.vertex_shader = skinned ? skinnedVertex : vertex;
            info.fragment_shader = fragment;
            info.vertex_input_state.vertex_buffer_descriptions = buffers;
            info.vertex_input_state.num_vertex_buffers = skinned ? 2 : 1;
            info.vertex_input_state.vertex_attributes = attributes;
            info.vertex_input_state.num_vertex_attributes = skinned ? 11 : 7;
            info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
            info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
            info.rasterizer_state.cull_mode = twoSided ? SDL_GPU_CULLMODE_NONE : SDL_GPU_CULLMODE_BACK;
            info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
            info.rasterizer_state.enable_depth_clip = clip != 0;
            info.depth_stencil_state.enable_depth_test = true;
            info.depth_stencil_state.enable_depth_write = blend == 0 || blend == 4;
            info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_GREATER_OR_EQUAL;
            info.target_info.color_target_descriptions = &color;
            info.target_info.num_color_targets = 1;
            info.multisample_state.sample_count = sampleCount(samples);
            info.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
            info.target_info.has_depth_stencil_target = true;
            auto& pipeline = (clip ? (skinned ? clippedSkinnedPipelines_ : clippedPipelines_) : (skinned ? out.skinnedScene : out.scene))[twoSided][blend];
            pipeline = SDL_CreateGPUGraphicsPipeline(device_, &info);
            if (pipeline == nullptr) {
                throw sdlError("pipeline");
            }
        }
    }
    SDL_ReleaseGPUShader(device_, vertex);
    SDL_ReleaseGPUShader(device_, skinnedVertex);
    SDL_ReleaseGPUShader(device_, fragment);
}

// Particles, one pipeline for each way the effect script's draw keyword asks for. Depth runs from 1 at the
// near plane to 0 at the far one, so the game's inverted test keeps what is farther than the depth drawn.
void SdlGpuRenderer::makeParticlePipelines(const std::filesystem::path& shaders, PassPipelines& out, int samples) {
    using Block = effects::ParticleBlock;
    SDL_GPUShader* vertex = loadShader(device_, shaders, "particle.vert", SDL_GPU_SHADERSTAGE_VERTEX, 0);
    SDL_GPUShader* fragment = loadShader(device_, shaders, "particle.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 1);
    SDL_GPUVertexBufferDescription buffer{};
    buffer.slot = 0;
    buffer.pitch = sizeof(scene::ParticleVertex);
    buffer.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    SDL_GPUVertexAttribute attributes[3]{};
    attributes[0] = {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(scene::ParticleVertex, position)};
    attributes[1] = {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(scene::ParticleVertex, uv)};
    attributes[2] = {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, offsetof(scene::ParticleVertex, colour)};
    for (int draw = 0; draw < static_cast<int>(out.particles.size()); ++draw) {
        const bool added = draw == Block::Additive || draw == Block::AdditiveInvertDepth || draw == Block::AdditiveIgnoreDepth;
        const bool inverted = draw == Block::DecalInvertDepth || draw == Block::AdditiveInvertDepth;
        const bool ignores = draw == Block::DecalIgnoreDepth || draw == Block::AdditiveIgnoreDepth;
        SDL_GPUColorTargetDescription color{};
        color.format = SDL_GetGPUSwapchainTextureFormat(device_, window_);
        color.blend_state.enable_blend = true;
        color.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
        color.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
        if (draw == Block::Modulate) {
            color.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_DST_COLOR;
            color.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
            color.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
            color.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        } else {
            color.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
            color.blend_state.dst_color_blendfactor = added ? SDL_GPU_BLENDFACTOR_ONE : SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
            color.blend_state.src_alpha_blendfactor = added ? SDL_GPU_BLENDFACTOR_ZERO : SDL_GPU_BLENDFACTOR_ONE;
            color.blend_state.dst_alpha_blendfactor = added ? SDL_GPU_BLENDFACTOR_ONE : SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        }
        SDL_GPUGraphicsPipelineCreateInfo info{};
        info.vertex_shader = vertex;
        info.fragment_shader = fragment;
        info.vertex_input_state.vertex_buffer_descriptions = &buffer;
        info.vertex_input_state.num_vertex_buffers = 1;
        info.vertex_input_state.vertex_attributes = attributes;
        info.vertex_input_state.num_vertex_attributes = 3;
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        info.rasterizer_state.enable_depth_clip = true;
        info.depth_stencil_state.enable_depth_test = !ignores;
        info.depth_stencil_state.enable_depth_write = draw == Block::DepthDecal;
        info.depth_stencil_state.compare_op = inverted ? SDL_GPU_COMPAREOP_LESS : SDL_GPU_COMPAREOP_GREATER_OR_EQUAL;
        info.target_info.color_target_descriptions = &color;
        info.target_info.num_color_targets = 1;
        info.multisample_state.sample_count = sampleCount(samples);
        info.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
        info.target_info.has_depth_stencil_target = true;
        out.particles[static_cast<std::size_t>(draw)] = SDL_CreateGPUGraphicsPipeline(device_, &info);
        if (out.particles[static_cast<std::size_t>(draw)] == nullptr) {
            SDL_ReleaseGPUShader(device_, vertex);
            SDL_ReleaseGPUShader(device_, fragment);
            throw sdlError("particle pipeline");
        }
    }
    SDL_ReleaseGPUShader(device_, vertex);
    SDL_ReleaseGPUShader(device_, fragment);
}

// Sprites: blended over everything, without depth.
void SdlGpuRenderer::makeSpritePipeline(const std::filesystem::path& shaders, PassPipelines& out, int samples) {
        SDL_GPUShader* spriteVertex = loadShader(device_, shaders, "ui.vert", SDL_GPU_SHADERSTAGE_VERTEX, 0);
        SDL_GPUShader* spriteFragment = loadShader(device_, shaders, "ui.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 1);
        SDL_GPUVertexBufferDescription buffer{};
        buffer.slot = 0;
        buffer.pitch = kSpriteFloats * sizeof(float);
        buffer.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
        SDL_GPUVertexAttribute attributes[3]{};
        attributes[0] = {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 0};
        attributes[1] = {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 2 * sizeof(float)};
        attributes[2] = {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 4 * sizeof(float)};
        SDL_GPUColorTargetDescription color{};
        color.format = SDL_GetGPUSwapchainTextureFormat(device_, window_);
        color.blend_state.enable_blend = true;
        color.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        color.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        color.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
        color.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        color.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        color.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
        SDL_GPUGraphicsPipelineCreateInfo info{};
        info.vertex_shader = spriteVertex;
        info.fragment_shader = spriteFragment;
        info.vertex_input_state.vertex_buffer_descriptions = &buffer;
        info.vertex_input_state.num_vertex_buffers = 1;
        info.vertex_input_state.vertex_attributes = attributes;
        info.vertex_input_state.num_vertex_attributes = 3;
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        info.target_info.color_target_descriptions = &color;
        info.target_info.num_color_targets = 1;
        info.multisample_state.sample_count = sampleCount(samples);
        info.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
        info.target_info.has_depth_stencil_target = true;
        out.sprite = SDL_CreateGPUGraphicsPipeline(device_, &info);
        SDL_ReleaseGPUShader(device_, spriteVertex);
        SDL_ReleaseGPUShader(device_, spriteFragment);
        if (out.sprite == nullptr) {
            throw sdlError("sprite pipeline");
        }
}

// The samplers of scene textures: repeating, and clamped at the edges.
void SdlGpuRenderer::makeSamplers() {
    SDL_GPUSamplerCreateInfo sampler{};
    sampler.min_filter = SDL_GPU_FILTER_LINEAR;
    sampler.mag_filter = SDL_GPU_FILTER_LINEAR;
    sampler.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    sampler.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    sampler.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    sampler.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    sampler.max_lod = 1000;
    sampler.enable_anisotropy = true;
    sampler.max_anisotropy = 8;
    sampler_ = SDL_CreateGPUSampler(device_, &sampler);
    // The same, clamped at the edges (materials whose textures don't repeat).
    sampler.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    clampedSampler_ = SDL_CreateGPUSampler(device_, &sampler);
    // Sprites, drawn whole.
    SDL_GPUSamplerCreateInfo clamped{};
    clamped.min_filter = SDL_GPU_FILTER_LINEAR;
    clamped.mag_filter = SDL_GPU_FILTER_LINEAR;
    clamped.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    clamped.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    clamped.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    clamped.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    spriteSampler_ = SDL_CreateGPUSampler(device_, &clamped);
    // Road textures, the same with mip levels.
    clamped.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    clamped.max_lod = 1000;
    clampedMips_ = SDL_CreateGPUSampler(device_, &clamped);
    // Imposter walls and slices.
    SDL_GPUSamplerCreateInfo point{};
    point.min_filter = SDL_GPU_FILTER_NEAREST;
    point.mag_filter = SDL_GPU_FILTER_NEAREST;
    point.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    point.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    point.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    point.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    pointSampler_ = SDL_CreateGPUSampler(device_, &point);
}

// Neighborhood terrain: opaque, both sides (as the game draws it).
void SdlGpuRenderer::makeTerrainPipeline(const std::filesystem::path& shaders, PassPipelines& out, int samples) {
        SDL_GPUShader* terrainVertex = loadShader(device_, shaders, "terrain.vert", SDL_GPU_SHADERSTAGE_VERTEX, 0);
        SDL_GPUShader* terrainFragment = loadShader(device_, shaders, "terrain.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 5);
        SDL_GPUVertexBufferDescription buffer{};
        buffer.slot = 0;
        buffer.pitch = 8 * sizeof(float);
        buffer.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
        SDL_GPUVertexAttribute attributes[3]{};
        attributes[0] = {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 0};
        attributes[1] = {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 3 * sizeof(float)};
        attributes[2] = {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 6 * sizeof(float)};
        SDL_GPUColorTargetDescription color{};
        color.format = SDL_GetGPUSwapchainTextureFormat(device_, window_);
        SDL_GPUGraphicsPipelineCreateInfo info{};
        info.vertex_shader = terrainVertex;
        info.fragment_shader = terrainFragment;
        info.vertex_input_state.vertex_buffer_descriptions = &buffer;
        info.vertex_input_state.num_vertex_buffers = 1;
        info.vertex_input_state.vertex_attributes = attributes;
        info.vertex_input_state.num_vertex_attributes = 3;
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        info.depth_stencil_state.enable_depth_test = true;
        info.depth_stencil_state.enable_depth_write = true;
        info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_GREATER_OR_EQUAL;
        info.target_info.color_target_descriptions = &color;
        info.target_info.num_color_targets = 1;
        info.multisample_state.sample_count = sampleCount(samples);
        info.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
        info.target_info.has_depth_stencil_target = true;
        out.terrain = SDL_CreateGPUGraphicsPipeline(device_, &info);
        SDL_ReleaseGPUShader(device_, terrainVertex);
        SDL_ReleaseGPUShader(device_, terrainFragment);
        if (out.terrain == nullptr) {
            throw sdlError("terrain pipeline");
        }
}

// Neighborhood roads: blended over the terrain they lie on.
void SdlGpuRenderer::makeRoadPipeline(const std::filesystem::path& shaders, PassPipelines& out, int samples) {
        SDL_GPUShader* roadVertex = loadShader(device_, shaders, "road.vert", SDL_GPU_SHADERSTAGE_VERTEX, 0);
        SDL_GPUShader* roadFragment = loadShader(device_, shaders, "road.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 2);
        SDL_GPUVertexBufferDescription buffer{};
        buffer.slot = 0;
        buffer.pitch = 6 * sizeof(float);
        buffer.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
        SDL_GPUVertexAttribute attributes[3]{};
        attributes[0] = {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 0};
        attributes[1] = {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 3 * sizeof(float)};
        attributes[2] = {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT, 5 * sizeof(float)};
        SDL_GPUColorTargetDescription color{};
        color.format = SDL_GetGPUSwapchainTextureFormat(device_, window_);
        color.blend_state.enable_blend = true;
        color.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        color.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        color.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
        color.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        color.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        color.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
        SDL_GPUGraphicsPipelineCreateInfo info{};
        info.vertex_shader = roadVertex;
        info.fragment_shader = roadFragment;
        info.vertex_input_state.vertex_buffer_descriptions = &buffer;
        info.vertex_input_state.num_vertex_buffers = 1;
        info.vertex_input_state.vertex_attributes = attributes;
        info.vertex_input_state.num_vertex_attributes = 3;
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        info.depth_stencil_state.enable_depth_test = true;
        info.depth_stencil_state.enable_depth_write = true;
        info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_GREATER_OR_EQUAL;
        info.target_info.color_target_descriptions = &color;
        info.target_info.num_color_targets = 1;
        info.multisample_state.sample_count = sampleCount(samples);
        info.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
        info.target_info.has_depth_stencil_target = true;
        out.road = SDL_CreateGPUGraphicsPipeline(device_, &info);
        SDL_ReleaseGPUShader(device_, roadVertex);
        SDL_ReleaseGPUShader(device_, roadFragment);
        if (out.road == nullptr) {
            throw sdlError("road pipeline");
        }
}

// Neighborhood lot imposters: one vertex layout, four ways of blending.
void SdlGpuRenderer::makeImposterPipelines(const std::filesystem::path& shaders, PassPipelines& out, int samples) {
        SDL_GPUShader* imposterVertex = loadShader(device_, shaders, "imposter.vert", SDL_GPU_SHADERSTAGE_VERTEX, 0);
        SDL_GPUShader* imposterFragment = loadShader(device_, shaders, "imposter.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 1);
        SDL_GPUVertexBufferDescription buffer{};
        buffer.slot = 0;
        buffer.pitch = 7 * sizeof(float);
        buffer.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
        SDL_GPUVertexAttribute attributes[3]{};
        attributes[0] = {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 0};
        attributes[1] = {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 3 * sizeof(float)};
        attributes[2] = {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 5 * sizeof(float)};
        auto make = [&](bool blend, SDL_GPUBlendFactor source, bool depthWrite, bool cull) {
            SDL_GPUColorTargetDescription color{};
            color.format = SDL_GetGPUSwapchainTextureFormat(device_, window_);
            color.blend_state.enable_blend = blend;
            color.blend_state.src_color_blendfactor = source;
            color.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
            color.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
            color.blend_state.src_alpha_blendfactor = source;
            color.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
            color.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
            SDL_GPUGraphicsPipelineCreateInfo info{};
            info.vertex_shader = imposterVertex;
            info.fragment_shader = imposterFragment;
            info.vertex_input_state.vertex_buffer_descriptions = &buffer;
            info.vertex_input_state.num_vertex_buffers = 1;
            info.vertex_input_state.vertex_attributes = attributes;
            info.vertex_input_state.num_vertex_attributes = 3;
            info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
            info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
            info.rasterizer_state.cull_mode = cull ? SDL_GPU_CULLMODE_BACK : SDL_GPU_CULLMODE_NONE;
            info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
            info.depth_stencil_state.enable_depth_test = true;
            info.depth_stencil_state.enable_depth_write = depthWrite;
            info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_GREATER_OR_EQUAL;
            info.target_info.color_target_descriptions = &color;
            info.target_info.num_color_targets = 1;
            info.multisample_state.sample_count = sampleCount(samples);
            info.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
            info.target_info.has_depth_stencil_target = true;
            SDL_GPUGraphicsPipeline* p = SDL_CreateGPUGraphicsPipeline(device_, &info);
            if (p == nullptr) {
                throw sdlError("imposter pipeline");
            }
            return p;
        };
        // Terrain: its light map multiplies what is under it, then its texture (alpha premultiplied)
        // goes over. Neither writes depth. Walls and slices: premultiplied, writing depth. Roofs:
        // opaque and two-sided. A fading imposter's walls and roofs blend by their alpha without
        // writing depth, as the game's imposter scripts draw them with a shape colour.
        out.imposterLight = make(true, SDL_GPU_BLENDFACTOR_DST_COLOR, false, true);
        out.imposterTerrain = make(true, SDL_GPU_BLENDFACTOR_ONE, false, true);
        out.imposterWall = make(true, SDL_GPU_BLENDFACTOR_ONE, true, true);
        out.imposterRoof = make(false, SDL_GPU_BLENDFACTOR_ONE, true, false);
        out.imposterFadedWall = make(true, SDL_GPU_BLENDFACTOR_SRC_ALPHA, false, true);
        out.imposterFadedRoof = make(true, SDL_GPU_BLENDFACTOR_SRC_ALPHA, false, false);
        SDL_ReleaseGPUShader(device_, imposterVertex);
        SDL_ReleaseGPUShader(device_, imposterFragment);
}

// A lot skirt's boundary curtain: one-sided walls written into depth, never into colour.
void SdlGpuRenderer::makeClipPipeline(const std::filesystem::path& shaders, PassPipelines& out, int samples) {
        SDL_GPUShader* clipVertex = loadShader(device_, shaders, "clip.vert", SDL_GPU_SHADERSTAGE_VERTEX, 0);
        SDL_GPUShader* clipFragment = loadShader(device_, shaders, "clip.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 0);
        SDL_GPUVertexBufferDescription buffer{};
        buffer.slot = 0;
        buffer.pitch = 3 * sizeof(float);
        buffer.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
        SDL_GPUVertexAttribute attribute{0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 0};
        SDL_GPUColorTargetDescription color{};
        color.format = SDL_GetGPUSwapchainTextureFormat(device_, window_);
        color.blend_state.enable_color_write_mask = true;
        color.blend_state.color_write_mask = 0;
        SDL_GPUGraphicsPipelineCreateInfo info{};
        info.vertex_shader = clipVertex;
        info.fragment_shader = clipFragment;
        info.vertex_input_state.vertex_buffer_descriptions = &buffer;
        info.vertex_input_state.num_vertex_buffers = 1;
        info.vertex_input_state.vertex_attributes = &attribute;
        info.vertex_input_state.num_vertex_attributes = 1;
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_BACK;
        info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
        info.depth_stencil_state.enable_depth_test = true;
        info.depth_stencil_state.enable_depth_write = true;
        info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_GREATER;
        info.target_info.color_target_descriptions = &color;
        info.target_info.num_color_targets = 1;
        info.multisample_state.sample_count = sampleCount(samples);
        info.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
        info.target_info.has_depth_stencil_target = true;
        out.clip = SDL_CreateGPUGraphicsPipeline(device_, &info);
        SDL_ReleaseGPUShader(device_, clipVertex);
        SDL_ReleaseGPUShader(device_, clipFragment);
        if (out.clip == nullptr) {
            throw sdlError("clip pipeline");
        }
}

// The sky seen in the neighborhood water: behind everything, no depth.
void SdlGpuRenderer::makeSkyPipeline(const std::filesystem::path& shaders, PassPipelines& out, int samples) {
        SDL_GPUShader* skyVertex = loadShader(device_, shaders, "sky.vert", SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
        SDL_GPUShader* skyFragment = loadShader(device_, shaders, "sky.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 1);
        SDL_GPUColorTargetDescription color{};
        color.format = SDL_GetGPUSwapchainTextureFormat(device_, window_);
        SDL_GPUGraphicsPipelineCreateInfo info{};
        info.vertex_shader = skyVertex;
        info.fragment_shader = skyFragment;
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        info.target_info.color_target_descriptions = &color;
        info.target_info.num_color_targets = 1;
        info.multisample_state.sample_count = sampleCount(samples);
        info.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
        info.target_info.has_depth_stencil_target = true;
        out.sky = SDL_CreateGPUGraphicsPipeline(device_, &info);
        SDL_ReleaseGPUShader(device_, skyVertex);
        SDL_ReleaseGPUShader(device_, skyFragment);
        if (out.sky == nullptr) {
            throw sdlError("sky pipeline");
        }
}

// The neighborhood water sheet: blended over what is below, testing depth without writing it.
void SdlGpuRenderer::makeWaterPipeline(const std::filesystem::path& shaders, PassPipelines& out, int samples) {
        SDL_GPUShader* waterVertex = loadShader(device_, shaders, "water.vert", SDL_GPU_SHADERSTAGE_VERTEX, 0);
        SDL_GPUShader* waterFragment = loadShader(device_, shaders, "water.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 2);
        SDL_GPUVertexBufferDescription buffer{};
        buffer.slot = 0;
        buffer.pitch = 5 * sizeof(float);
        buffer.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
        SDL_GPUVertexAttribute attributes[2]{};
        attributes[0] = {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 0};
        attributes[1] = {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 3 * sizeof(float)};
        SDL_GPUColorTargetDescription color{};
        color.format = SDL_GetGPUSwapchainTextureFormat(device_, window_);
        color.blend_state.enable_blend = true;
        color.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        color.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        color.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
        color.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        color.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        color.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
        SDL_GPUGraphicsPipelineCreateInfo info{};
        info.vertex_shader = waterVertex;
        info.fragment_shader = waterFragment;
        info.vertex_input_state.vertex_buffer_descriptions = &buffer;
        info.vertex_input_state.num_vertex_buffers = 1;
        info.vertex_input_state.vertex_attributes = attributes;
        info.vertex_input_state.num_vertex_attributes = 2;
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        info.depth_stencil_state.enable_depth_test = true;
        info.depth_stencil_state.enable_depth_write = false;
        info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_GREATER_OR_EQUAL;
        info.target_info.color_target_descriptions = &color;
        info.target_info.num_color_targets = 1;
        info.multisample_state.sample_count = sampleCount(samples);
        info.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
        info.target_info.has_depth_stencil_target = true;
        out.water = SDL_CreateGPUGraphicsPipeline(device_, &info);
        SDL_ReleaseGPUShader(device_, waterVertex);
        SDL_ReleaseGPUShader(device_, waterFragment);
        if (out.water == nullptr) {
            throw sdlError("water pipeline");
        }
}

// The water in a lot's ponds: the scene's positions, normals, texture coordinates and alphas,
// blended (premultiplied) over what is below and writing depth, as the game's first water pass does.
void SdlGpuRenderer::makePondPipeline(const std::filesystem::path& shaders, PassPipelines& out, int samples) {
    SDL_GPUShader* vertex = loadShader(device_, shaders, "pond.vert", SDL_GPU_SHADERSTAGE_VERTEX, 0);
    SDL_GPUShader* fragment = loadShader(device_, shaders, "pond.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 3, 1);
    SDL_GPUVertexBufferDescription buffers[2]{};
    SDL_GPUVertexAttribute attributes[11]{};
    sceneVertexLayout(buffers, attributes);
    SDL_GPUColorTargetDescription color{};
    color.format = SDL_GetGPUSwapchainTextureFormat(device_, window_);
    color.blend_state.enable_blend = true;
    color.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    color.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    color.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
    color.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    color.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    color.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    SDL_GPUGraphicsPipelineCreateInfo info{};
    info.vertex_shader = vertex;
    info.fragment_shader = fragment;
    info.vertex_input_state.vertex_buffer_descriptions = buffers;
    info.vertex_input_state.num_vertex_buffers = 1;
    info.vertex_input_state.vertex_attributes = attributes;
    info.vertex_input_state.num_vertex_attributes = 4;
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    info.depth_stencil_state.enable_depth_test = true;
    info.depth_stencil_state.enable_depth_write = true;
    info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_GREATER_OR_EQUAL;
    info.target_info.color_target_descriptions = &color;
    info.target_info.num_color_targets = 1;
    info.multisample_state.sample_count = sampleCount(samples);
    info.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    info.target_info.has_depth_stencil_target = true;
    out.pond = SDL_CreateGPUGraphicsPipeline(device_, &info);
    SDL_ReleaseGPUShader(device_, vertex);
    SDL_ReleaseGPUShader(device_, fragment);
    if (out.pond == nullptr) {
        throw sdlError("pond pipeline");
    }
}

// A pool's water surface: the scene's positions and normals, blended over what is below, testing
// depth without writing it, as the game's pool water material does.
void SdlGpuRenderer::makePoolPipeline(const std::filesystem::path& shaders, PassPipelines& out, int samples, bool gameAlpha) {
    SDL_GPUShader* vertex = loadShader(device_, shaders, "pool.vert", SDL_GPU_SHADERSTAGE_VERTEX, 0);
    SDL_GPUShader* fragment = loadShader(device_, shaders, "pool.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 2, 0);
    SDL_GPUVertexBufferDescription buffers[2]{};
    SDL_GPUVertexAttribute attributes[11]{};
    sceneVertexLayout(buffers, attributes);
    SDL_GPUColorTargetDescription color{};
    color.format = SDL_GetGPUSwapchainTextureFormat(device_, window_);
    color.blend_state.enable_blend = true;
    color.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    color.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    color.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
    color.blend_state.src_alpha_blendfactor = gameAlpha ? SDL_GPU_BLENDFACTOR_SRC_ALPHA : SDL_GPU_BLENDFACTOR_ONE;
    color.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    color.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    SDL_GPUGraphicsPipelineCreateInfo info{};
    info.vertex_shader = vertex;
    info.fragment_shader = fragment;
    info.vertex_input_state.vertex_buffer_descriptions = buffers;
    info.vertex_input_state.num_vertex_buffers = 1;
    info.vertex_input_state.vertex_attributes = attributes;
    info.vertex_input_state.num_vertex_attributes = 2;
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_BACK;
    info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    info.depth_stencil_state.enable_depth_test = true;
    info.depth_stencil_state.enable_depth_write = false;
    info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_GREATER_OR_EQUAL;
    info.target_info.color_target_descriptions = &color;
    info.target_info.num_color_targets = 1;
    info.multisample_state.sample_count = sampleCount(samples);
    info.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    info.target_info.has_depth_stencil_target = true;
    out.pool = SDL_CreateGPUGraphicsPipeline(device_, &info);
    SDL_ReleaseGPUShader(device_, vertex);
    SDL_ReleaseGPUShader(device_, fragment);
    if (out.pool == nullptr) {
        throw sdlError("pool pipeline");
    }
}

// A beach lot's sea: the scene's positions, normals, texture coordinates and alphas, blended over
// what is below, testing depth without writing it, as the game's lotSkirtSea material does.
void SdlGpuRenderer::makeSeaPipeline(const std::filesystem::path& shaders, PassPipelines& out, int samples) {
    SDL_GPUShader* vertex = loadShader(device_, shaders, "sea.vert", SDL_GPU_SHADERSTAGE_VERTEX, 0);
    SDL_GPUShader* fragment = loadShader(device_, shaders, "sea.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 2, 0);
    SDL_GPUVertexBufferDescription buffers[2]{};
    SDL_GPUVertexAttribute attributes[11]{};
    sceneVertexLayout(buffers, attributes);
    SDL_GPUColorTargetDescription color{};
    color.format = SDL_GetGPUSwapchainTextureFormat(device_, window_);
    color.blend_state.enable_blend = true;
    color.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    color.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    color.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
    color.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    color.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    color.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    SDL_GPUGraphicsPipelineCreateInfo info{};
    info.vertex_shader = vertex;
    info.fragment_shader = fragment;
    info.vertex_input_state.vertex_buffer_descriptions = buffers;
    info.vertex_input_state.num_vertex_buffers = 1;
    info.vertex_input_state.vertex_attributes = attributes;
    info.vertex_input_state.num_vertex_attributes = 4;
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_BACK;
    info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    info.depth_stencil_state.enable_depth_test = true;
    info.depth_stencil_state.enable_depth_write = false;
    info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_GREATER_OR_EQUAL;
    info.target_info.color_target_descriptions = &color;
    info.target_info.num_color_targets = 1;
    info.multisample_state.sample_count = sampleCount(samples);
    info.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    info.target_info.has_depth_stencil_target = true;
    out.sea = SDL_CreateGPUGraphicsPipeline(device_, &info);
    SDL_ReleaseGPUShader(device_, vertex);
    SDL_ReleaseGPUShader(device_, fragment);
    if (out.sea == nullptr) {
        throw sdlError("sea pipeline");
    }
}

// Ambient occlusion: its depth pass, then how open each pixel is and its blur, over the whole target.
void SdlGpuRenderer::makeOcclusionPipelines() {
    const std::filesystem::path& shaders = shaderFolder_;
    SDL_GPUShader* vertex = loadShader(device_, shaders, "scene.vert", SDL_GPU_SHADERSTAGE_VERTEX, 0);
    SDL_GPUShader* skinnedVertex = loadShader(device_, shaders, "skinned.vert", SDL_GPU_SHADERSTAGE_VERTEX, 0, 1, 2);
    SDL_GPUShader* depthFragment = loadShader(device_, shaders, "ao_depth.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 2);
    SDL_GPUShader* solidFragment = loadShader(device_, shaders, "ao_solid.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 0);
    SDL_GPUShader* screenVertex = loadShader(device_, shaders, "sky.vert", SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
    SDL_GPUShader* occlusionFragment = loadShader(device_, shaders, "ao.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 1);
    SDL_GPUShader* blurFragment = loadShader(device_, shaders, "ao_blur.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 2, 0);
    SDL_GPUShader* upsampleFragment = loadShader(device_, shaders, "ao_upsample.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 3, 0);
    SDL_GPUShader* halfDepthFragment = loadShader(device_, shaders, "ao_halfdepth.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 0);

    SDL_GPUVertexBufferDescription buffers[2]{};
    SDL_GPUVertexAttribute attributes[11]{};
    sceneVertexLayout(buffers, attributes);
    for (int skinned = 0; skinned < 2; ++skinned) {
        for (int twoSided = 0; twoSided < 2; ++twoSided) {
            SDL_GPUGraphicsPipelineCreateInfo info{};
            info.vertex_shader = skinned ? skinnedVertex : vertex;
            info.fragment_shader = depthFragment;
            info.vertex_input_state.vertex_buffer_descriptions = buffers;
            info.vertex_input_state.num_vertex_buffers = skinned ? 2 : 1;
            info.vertex_input_state.vertex_attributes = attributes;
            info.vertex_input_state.num_vertex_attributes = skinned ? 11 : 7;
            info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
            info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
            info.rasterizer_state.cull_mode = twoSided ? SDL_GPU_CULLMODE_NONE : SDL_GPU_CULLMODE_BACK;
            info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
            info.depth_stencil_state.enable_depth_test = true;
            info.depth_stencil_state.enable_depth_write = true;
            info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_GREATER_OR_EQUAL;
            info.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
            info.target_info.has_depth_stencil_target = true;
            occlusion_.depthPipelines[skinned][twoSided] = SDL_CreateGPUGraphicsPipeline(device_, &info);
            info.fragment_shader = solidFragment;
            occlusion_.solidPipelines[skinned][twoSided] = SDL_CreateGPUGraphicsPipeline(device_, &info);
        }
    }
    SDL_GPUColorTargetDescription color{};
    color.format = kOcclusionFormat;
    SDL_GPUGraphicsPipelineCreateInfo info{};
    info.vertex_shader = screenVertex;
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    info.target_info.color_target_descriptions = &color;
    info.target_info.num_color_targets = 1;
    info.fragment_shader = occlusionFragment;
    occlusion_.occlusionPipeline = SDL_CreateGPUGraphicsPipeline(device_, &info);
    info.fragment_shader = blurFragment;
    occlusion_.blurPipeline = SDL_CreateGPUGraphicsPipeline(device_, &info);
    info.fragment_shader = upsampleFragment;
    occlusion_.upsamplePipeline = SDL_CreateGPUGraphicsPipeline(device_, &info);
    color.format = kHalfDepthFormat;
    info.fragment_shader = halfDepthFragment;
    occlusion_.halfDepthPipeline = SDL_CreateGPUGraphicsPipeline(device_, &info);
    for (auto* s : {vertex, skinnedVertex, depthFragment, solidFragment, screenVertex, occlusionFragment, blurFragment, upsampleFragment, halfDepthFragment}) {
        SDL_ReleaseGPUShader(device_, s);
    }
    bool all = occlusion_.occlusionPipeline != nullptr && occlusion_.blurPipeline != nullptr && occlusion_.upsamplePipeline != nullptr &&
               occlusion_.halfDepthPipeline != nullptr;
    for (auto* rows : {occlusion_.depthPipelines, occlusion_.solidPipelines}) {
        for (int skinned = 0; skinned < 2; ++skinned) {
            all = all && rows[skinned][0] != nullptr && rows[skinned][1] != nullptr;
        }
    }
    if (!all) {
        throw sdlError("ambient occlusion pipeline");
    }
}

// The pools' caustics: the tile drawn over its whole target, and the shadow map, where the nearest
// depth along the sun wins.
void SdlGpuRenderer::makeCausticsPipelines(const std::filesystem::path& shaders) {
    constexpr SDL_GPUTextureFormat kTile = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, kDepth = SDL_GPU_TEXTUREFORMAT_R32_FLOAT;
    SDL_GPUShader* tileVertex = loadShader(device_, shaders, "sky.vert", SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
    SDL_GPUShader* tileFragment = loadShader(device_, shaders, "caustics.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 1);
    SDL_GPUColorTargetDescription tile{};
    tile.format = kTile;
    SDL_GPUGraphicsPipelineCreateInfo info{};
    info.vertex_shader = tileVertex;
    info.fragment_shader = tileFragment;
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    info.target_info.color_target_descriptions = &tile;
    info.target_info.num_color_targets = 1;
    causticsPipeline_ = SDL_CreateGPUGraphicsPipeline(device_, &info);

    SDL_GPUShader* vertex = loadShader(device_, shaders, "scene.vert", SDL_GPU_SHADERSTAGE_VERTEX, 0);
    SDL_GPUShader* skinnedVertex = loadShader(device_, shaders, "skinned.vert", SDL_GPU_SHADERSTAGE_VERTEX, 0, 1, 2);
    SDL_GPUShader* depthFragment = loadShader(device_, shaders, "pool_shadow.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 0);
    SDL_GPUColorTargetDescription depth{};
    depth.format = kDepth;
    SDL_GPUVertexBufferDescription buffers[2]{};
    SDL_GPUVertexAttribute attributes[11]{};
    sceneVertexLayout(buffers, attributes);
    for (int skinned = 0; skinned < 2; ++skinned) {
        info = {};
        info.vertex_shader = skinned ? skinnedVertex : vertex;
        info.fragment_shader = depthFragment;
        info.vertex_input_state.vertex_buffer_descriptions = buffers;
        info.vertex_input_state.num_vertex_buffers = skinned ? 2 : 1;
        info.vertex_input_state.vertex_attributes = attributes;
        info.vertex_input_state.num_vertex_attributes = skinned ? 11 : 7;
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        info.rasterizer_state.enable_depth_clip = true;
        info.depth_stencil_state.enable_depth_test = true;
        info.depth_stencil_state.enable_depth_write = true;
        info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS;
        info.target_info.color_target_descriptions = &depth;
        info.target_info.num_color_targets = 1;
        info.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
        info.target_info.has_depth_stencil_target = true;
        (skinned ? poolShadowSkinnedPipeline_ : poolShadowPipeline_) = SDL_CreateGPUGraphicsPipeline(device_, &info);
    }
    for (auto* s : {tileVertex, tileFragment, vertex, skinnedVertex, depthFragment}) {
        SDL_ReleaseGPUShader(device_, s);
    }
    if (causticsPipeline_ == nullptr || poolShadowPipeline_ == nullptr || poolShadowSkinnedPipeline_ == nullptr) {
        throw sdlError("caustics pipeline");
    }
    SDL_GPUTextureCreateInfo texture{};
    texture.type = SDL_GPU_TEXTURETYPE_2D;
    texture.layer_count_or_depth = 1;
    texture.num_levels = 1;
    texture.format = kTile;
    texture.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    texture.width = texture.height = kCausticsTile;
    causticsTile_ = SDL_CreateGPUTexture(device_, &texture);
    texture.format = kDepth;
    texture.width = texture.height = kPoolShadowMap;
    poolShadow_ = SDL_CreateGPUTexture(device_, &texture);
    texture.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    texture.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
    poolShadowDepth_ = SDL_CreateGPUTexture(device_, &texture);
    if (causticsTile_ == nullptr || poolShadow_ == nullptr || poolShadowDepth_ == nullptr) {
        throw sdlError("caustics textures");
    }
}

// The Sims' shadow pictures: each Sim drawn flat into its tile without depth, then the tiles blurred
// into a second atlas. The atlases are made once.
void SdlGpuRenderer::makeShadowPipelines(const std::filesystem::path& shaders) {
    constexpr SDL_GPUTextureFormat kFormat = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    SDL_GPUShader* casterVertex = loadShader(device_, shaders, "skinned.vert", SDL_GPU_SHADERSTAGE_VERTEX, 0, 1, 2);
    SDL_GPUShader* casterFragment = loadShader(device_, shaders, "shadow_caster.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 0);
    SDL_GPUShader* blurVertex = loadShader(device_, shaders, "sky.vert", SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
    SDL_GPUShader* blurFragment = loadShader(device_, shaders, "shadow_blur.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 1);
    SDL_GPUColorTargetDescription color{};
    color.format = kFormat;
    SDL_GPUVertexBufferDescription buffers[2]{};
    SDL_GPUVertexAttribute attributes[11]{};
    sceneVertexLayout(buffers, attributes);
    SDL_GPUGraphicsPipelineCreateInfo info{};
    info.vertex_shader = casterVertex;
    info.fragment_shader = casterFragment;
    info.vertex_input_state.vertex_buffer_descriptions = buffers;
    info.vertex_input_state.num_vertex_buffers = 2;
    info.vertex_input_state.vertex_attributes = attributes;
    info.vertex_input_state.num_vertex_attributes = 11;
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    info.target_info.color_target_descriptions = &color;
    info.target_info.num_color_targets = 1;
    shadowCasterPipeline_ = SDL_CreateGPUGraphicsPipeline(device_, &info);
    info = {};
    info.vertex_shader = blurVertex;
    info.fragment_shader = blurFragment;
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    info.target_info.color_target_descriptions = &color;
    info.target_info.num_color_targets = 1;
    shadowBlurPipeline_ = SDL_CreateGPUGraphicsPipeline(device_, &info);
    for (auto* s : {casterVertex, casterFragment, blurVertex, blurFragment}) {
        SDL_ReleaseGPUShader(device_, s);
    }
    if (shadowCasterPipeline_ == nullptr || shadowBlurPipeline_ == nullptr) {
        throw sdlError("shadow pipeline");
    }
    SDL_GPUTextureCreateInfo texture{};
    texture.type = SDL_GPU_TEXTURETYPE_2D;
    texture.format = kFormat;
    texture.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    // shadow_blur.frag and scene.frag are written for this size: 4 tiles across of 128 texels.
    static_assert(scene::simshadows::kResolution == 128 && kShadowTilesAcross == 4);
    texture.width = texture.height = scene::simshadows::kResolution * kShadowTilesAcross;
    texture.layer_count_or_depth = 1;
    texture.num_levels = 1;
    shadowPictures_ = SDL_CreateGPUTexture(device_, &texture);
    shadowBlurred_ = SDL_CreateGPUTexture(device_, &texture);
    SDL_GPUSamplerCreateInfo sampler{};
    sampler.min_filter = SDL_GPU_FILTER_LINEAR;
    sampler.mag_filter = SDL_GPU_FILTER_LINEAR;
    sampler.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    sampler.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    shadowSampler_ = SDL_CreateGPUSampler(device_, &sampler);
    sampler.min_filter = SDL_GPU_FILTER_NEAREST;
    sampler.mag_filter = SDL_GPU_FILTER_NEAREST;
    shadowPoint_ = SDL_CreateGPUSampler(device_, &sampler);
    if (shadowPictures_ == nullptr || shadowBlurred_ == nullptr || shadowSampler_ == nullptr || shadowPoint_ == nullptr) {
        throw sdlError("shadow pictures");
    }
    makeObjectShadowPipelines(shaders);
}

// The lot objects' pictures from the sun: their models drawn flat into a scratch picture, then
// blurred into the atlas, and the list and grid the scene's fragment shader reads.
void SdlGpuRenderer::makeObjectShadowPipelines(const std::filesystem::path& shaders) {
    constexpr SDL_GPUTextureFormat kFormat = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    SDL_GPUShader* casterVertex = loadShader(device_, shaders, "scene.vert", SDL_GPU_SHADERSTAGE_VERTEX, 0);
    SDL_GPUShader* casterFragment = loadShader(device_, shaders, "shadow_caster.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 0);
    SDL_GPUShader* blurVertex = loadShader(device_, shaders, "sky.vert", SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
    SDL_GPUShader* blurFragment = loadShader(device_, shaders, "object_blur.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
    SDL_GPUColorTargetDescription color{};
    color.format = kFormat;
    SDL_GPUVertexBufferDescription buffers[2]{};
    SDL_GPUVertexAttribute attributes[11]{};
    sceneVertexLayout(buffers, attributes);
    SDL_GPUGraphicsPipelineCreateInfo info{};
    info.vertex_shader = casterVertex;
    info.fragment_shader = casterFragment;
    info.vertex_input_state.vertex_buffer_descriptions = buffers;
    info.vertex_input_state.num_vertex_buffers = 1;
    info.vertex_input_state.vertex_attributes = attributes;
    info.vertex_input_state.num_vertex_attributes = 7;
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    info.target_info.color_target_descriptions = &color;
    info.target_info.num_color_targets = 1;
    objectCasterPipeline_ = SDL_CreateGPUGraphicsPipeline(device_, &info);
    info = {};
    info.vertex_shader = blurVertex;
    info.fragment_shader = blurFragment;
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    info.target_info.color_target_descriptions = &color;
    info.target_info.num_color_targets = 1;
    objectBlurPipeline_ = SDL_CreateGPUGraphicsPipeline(device_, &info);
    for (auto* s : {casterVertex, casterFragment, blurVertex, blurFragment}) {
        SDL_ReleaseGPUShader(device_, s);
    }
    if (objectCasterPipeline_ == nullptr || objectBlurPipeline_ == nullptr) {
        throw sdlError("object shadow pipeline");
    }
    SDL_GPUTextureCreateInfo texture{};
    texture.type = SDL_GPU_TEXTURETYPE_2D;
    texture.format = kFormat;
    texture.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    texture.layer_count_or_depth = 1;
    texture.num_levels = 1;
    texture.width = texture.height = scene::simshadows::kResolution;
    objectScratch_ = SDL_CreateGPUTexture(device_, &texture);
    // object_blur.frag and scene.frag are written for this size: 2048 texels square.
    static_assert(scene::simshadows::kResolution * kObjectTilesAcross == 2048);
    texture.width = texture.height = scene::simshadows::kResolution * kObjectTilesAcross;
    objectPictures_ = SDL_CreateGPUTexture(device_, &texture);
    texture.format = SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT;
    texture.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    texture.width = kObjectPlaceTexels;
    texture.height = static_cast<Uint32>(kMaxObjectPlaces);
    objectPlaces_ = SDL_CreateGPUTexture(device_, &texture);
    texture.width = 2 * kMaxObjectGrid;
    texture.height = kMaxObjectGrid;
    objectGrid_ = SDL_CreateGPUTexture(device_, &texture);
    texture.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    texture.width = kMaxObjectFloorTiles;
    texture.height = kMaxObjectFloorTiles * kMaxObjectFloorLevels;
    objectFloors_ = SDL_CreateGPUTexture(device_, &texture);
    if (objectScratch_ == nullptr || objectPictures_ == nullptr || objectPlaces_ == nullptr || objectGrid_ == nullptr || objectFloors_ == nullptr) {
        throw sdlError("object shadow pictures");
    }
}

} // namespace ts2::render
