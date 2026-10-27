#include "render/renderer.hpp"
#include "render/renderer_internals.hpp"

#include <SDL3/SDL.h>

#include <fstream>
#include <stdexcept>

namespace ts2::render {

using namespace render_detail;

namespace {

std::vector<std::uint8_t> readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("can't read " + path.string());
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

} // namespace

void SdlGpuRenderer::create(const std::filesystem::path& shaderFolder) {
    makeScenePipelines(shaderFolder);
    makeSpritePipeline(shaderFolder);
    makeSamplers();
    makeTerrainPipeline(shaderFolder);
    makeRoadPipeline(shaderFolder);
    makeImposterPipelines(shaderFolder);
    makeClipPipeline(shaderFolder);
    makeSkyPipeline(shaderFolder);
    makeWaterPipeline(shaderFolder);
}

// The scene's pipelines, by skinned, two-sided and blending.
void SdlGpuRenderer::makeScenePipelines(const std::filesystem::path& shaders) {
    SDL_GPUShader* vertex = loadShader(device_, shaders, "scene.vert", SDL_GPU_SHADERSTAGE_VERTEX, 0);
    SDL_GPUShader* skinnedVertex = loadShader(device_, shaders, "skinned.vert", SDL_GPU_SHADERSTAGE_VERTEX, 0, 1, 1);
    SDL_GPUShader* fragment = loadShader(device_, shaders, "scene.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 6);

    for (int skinned = 0; skinned < 2; ++skinned)
    for (int twoSided = 0; twoSided < 2; ++twoSided) {
        for (int blend = 0; blend < 3; ++blend) {
            // Skinned primitives take their joints and weights from a second buffer.
            SDL_GPUVertexBufferDescription buffers[2]{};
            buffers[0].slot = 0;
            buffers[0].pitch = kVertexFloats * sizeof(float);
            buffers[0].input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
            buffers[1].slot = 1;
            buffers[1].pitch = kSkinBytes;
            buffers[1].input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
            SDL_GPUVertexAttribute attributes[9]{};
            attributes[0] = {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 0};
            attributes[1] = {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 3 * sizeof(float)};
            attributes[2] = {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 6 * sizeof(float)};
            attributes[3] = {3, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT, 8 * sizeof(float)};
            attributes[4] = {6, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 9 * sizeof(float)};
            attributes[5] = {7, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 11 * sizeof(float)};
            attributes[6] = {8, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 14 * sizeof(float)};
            attributes[7] = {4, 1, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4, 0};
            attributes[8] = {5, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 4};

            SDL_GPUColorTargetDescription color{};
            color.format = SDL_GetGPUSwapchainTextureFormat(device_, window_);
            if (blend) {
                color.blend_state.enable_blend = true;
                color.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
                color.blend_state.dst_color_blendfactor = blend == 2 ? SDL_GPU_BLENDFACTOR_ONE : SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
                color.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
                color.blend_state.src_alpha_blendfactor = blend == 2 ? SDL_GPU_BLENDFACTOR_ZERO : SDL_GPU_BLENDFACTOR_ONE;
                color.blend_state.dst_alpha_blendfactor = blend == 2 ? SDL_GPU_BLENDFACTOR_ONE : SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
                color.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
            }

            SDL_GPUGraphicsPipelineCreateInfo info{};
            info.vertex_shader = skinned ? skinnedVertex : vertex;
            info.fragment_shader = fragment;
            info.vertex_input_state.vertex_buffer_descriptions = buffers;
            info.vertex_input_state.num_vertex_buffers = skinned ? 2 : 1;
            info.vertex_input_state.vertex_attributes = attributes;
            info.vertex_input_state.num_vertex_attributes = skinned ? 9 : 7;
            info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
            info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
            info.rasterizer_state.cull_mode = twoSided ? SDL_GPU_CULLMODE_NONE : SDL_GPU_CULLMODE_BACK;
            info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
            info.depth_stencil_state.enable_depth_test = true;
            info.depth_stencil_state.enable_depth_write = !blend;
            info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_GREATER_OR_EQUAL;
            info.target_info.color_target_descriptions = &color;
            info.target_info.num_color_targets = 1;
            info.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
            info.target_info.has_depth_stencil_target = true;
            auto& pipeline = (skinned ? skinnedPipelines_ : pipelines_)[twoSided][blend];
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

// Sprites: blended over everything, without depth.
void SdlGpuRenderer::makeSpritePipeline(const std::filesystem::path& shaders) {
        SDL_GPUShader* spriteVertex = loadShader(device_, shaders, "ui.vert", SDL_GPU_SHADERSTAGE_VERTEX, 0);
        SDL_GPUShader* spriteFragment = loadShader(device_, shaders, "ui.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 0);
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
        info.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
        info.target_info.has_depth_stencil_target = true;
        spritePipeline_ = SDL_CreateGPUGraphicsPipeline(device_, &info);
        SDL_ReleaseGPUShader(device_, spriteVertex);
        SDL_ReleaseGPUShader(device_, spriteFragment);
        if (spritePipeline_ == nullptr) {
            throw sdlError("sprite pipeline");
        }
        SDL_GPUSamplerCreateInfo clamped{};
        clamped.min_filter = SDL_GPU_FILTER_LINEAR;
        clamped.mag_filter = SDL_GPU_FILTER_LINEAR;
        clamped.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
        clamped.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        clamped.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        clamped.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        spriteSampler_ = SDL_CreateGPUSampler(device_, &clamped);
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
}

// Neighborhood terrain: opaque, both sides (as the game draws it).
void SdlGpuRenderer::makeTerrainPipeline(const std::filesystem::path& shaders) {
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
        info.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
        info.target_info.has_depth_stencil_target = true;
        terrainPipeline_ = SDL_CreateGPUGraphicsPipeline(device_, &info);
        SDL_ReleaseGPUShader(device_, terrainVertex);
        SDL_ReleaseGPUShader(device_, terrainFragment);
        if (terrainPipeline_ == nullptr) {
            throw sdlError("terrain pipeline");
        }
}

// Neighborhood roads: blended over the terrain they lie on.
void SdlGpuRenderer::makeRoadPipeline(const std::filesystem::path& shaders) {
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
        info.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
        info.target_info.has_depth_stencil_target = true;
        roadPipeline_ = SDL_CreateGPUGraphicsPipeline(device_, &info);
        SDL_ReleaseGPUShader(device_, roadVertex);
        SDL_ReleaseGPUShader(device_, roadFragment);
        if (roadPipeline_ == nullptr) {
            throw sdlError("road pipeline");
        }
        SDL_GPUSamplerCreateInfo clamped{};
        clamped.min_filter = SDL_GPU_FILTER_LINEAR;
        clamped.mag_filter = SDL_GPU_FILTER_LINEAR;
        clamped.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
        clamped.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        clamped.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        clamped.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        clamped.max_lod = 1000;
        clampedMips_ = SDL_CreateGPUSampler(device_, &clamped);
}

// Neighborhood lot imposters: one vertex layout, four ways of blending.
void SdlGpuRenderer::makeImposterPipelines(const std::filesystem::path& shaders) {
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
        // opaque and two-sided.
        imposterLightPipeline_ = make(true, SDL_GPU_BLENDFACTOR_DST_COLOR, false, true);
        imposterTerrainPipeline_ = make(true, SDL_GPU_BLENDFACTOR_ONE, false, true);
        imposterWallPipeline_ = make(true, SDL_GPU_BLENDFACTOR_ONE, true, true);
        imposterRoofPipeline_ = make(false, SDL_GPU_BLENDFACTOR_ONE, true, false);
        SDL_ReleaseGPUShader(device_, imposterVertex);
        SDL_ReleaseGPUShader(device_, imposterFragment);
        SDL_GPUSamplerCreateInfo point{};
        point.min_filter = SDL_GPU_FILTER_NEAREST;
        point.mag_filter = SDL_GPU_FILTER_NEAREST;
        point.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
        point.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
        point.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
        point.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
        pointSampler_ = SDL_CreateGPUSampler(device_, &point);
}

// A lot skirt's boundary curtain: one-sided walls written into depth, never into colour.
void SdlGpuRenderer::makeClipPipeline(const std::filesystem::path& shaders) {
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
        info.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
        info.target_info.has_depth_stencil_target = true;
        clipPipeline_ = SDL_CreateGPUGraphicsPipeline(device_, &info);
        SDL_ReleaseGPUShader(device_, clipVertex);
        SDL_ReleaseGPUShader(device_, clipFragment);
        if (clipPipeline_ == nullptr) {
            throw sdlError("clip pipeline");
        }
}

// The sky seen in the neighborhood water: behind everything, no depth.
void SdlGpuRenderer::makeSkyPipeline(const std::filesystem::path& shaders) {
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
        info.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
        info.target_info.has_depth_stencil_target = true;
        skyPipeline_ = SDL_CreateGPUGraphicsPipeline(device_, &info);
        SDL_ReleaseGPUShader(device_, skyVertex);
        SDL_ReleaseGPUShader(device_, skyFragment);
        if (skyPipeline_ == nullptr) {
            throw sdlError("sky pipeline");
        }
}

// The neighborhood water sheet: blended over what is below, testing depth without writing it.
void SdlGpuRenderer::makeWaterPipeline(const std::filesystem::path& shaders) {
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
        info.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
        info.target_info.has_depth_stencil_target = true;
        waterPipeline_ = SDL_CreateGPUGraphicsPipeline(device_, &info);
        SDL_ReleaseGPUShader(device_, waterVertex);
        SDL_ReleaseGPUShader(device_, waterFragment);
        if (waterPipeline_ == nullptr) {
            throw sdlError("water pipeline");
        }
}

} // namespace ts2::render
