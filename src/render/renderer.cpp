#include "render/renderer.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace ts2::render {

namespace {

constexpr std::uint32_t kVertexFloats = 9;  // position, normal, texture coordinates, alpha
constexpr std::uint32_t kSpriteFloats = 8;  // position, texture coordinates, color

struct VertexUniforms {
    float viewProjection[16];
    float model[16];
};

struct FragmentUniforms {
    float color[4];
    float sunDirection[4];
    float hasTexture;
    float alphaCutoff;
    float unlit;
    float padding;
};

std::vector<std::uint8_t> readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("can't read " + path.string());
    }
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::runtime_error sdlError(const char* what) {
    return std::runtime_error(std::string(what) + ": " + SDL_GetError());
}

} // namespace

Renderer::Renderer(SDL_Window* window, const std::filesystem::path& shaderFolder) : window_(window) {
    device_ = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_DXIL, false, nullptr);
    if (device_ == nullptr) {
        throw sdlError("no GPU device");
    }
    if (!SDL_ClaimWindowForGPUDevice(device_, window_)) {
        throw sdlError("can't draw to the window");
    }

    auto shader = [&](const char* file, SDL_GPUShaderStage stage, Uint32 samplers, Uint32 uniforms = 1) {
        const auto code = readFile(shaderFolder / file);
        SDL_GPUShaderCreateInfo info{};
        info.code_size = code.size();
        info.code = code.data();
        info.entrypoint = "main";
        info.format = SDL_GPU_SHADERFORMAT_DXIL;
        info.stage = stage;
        info.num_samplers = samplers;
        info.num_uniform_buffers = uniforms;
        SDL_GPUShader* s = SDL_CreateGPUShader(device_, &info);
        if (s == nullptr) {
            throw sdlError(file);
        }
        return s;
    };
    SDL_GPUShader* vertex = shader("scene.vert.dxil", SDL_GPU_SHADERSTAGE_VERTEX, 0);
    SDL_GPUShader* fragment = shader("scene.frag.dxil", SDL_GPU_SHADERSTAGE_FRAGMENT, 1);

    for (int twoSided = 0; twoSided < 2; ++twoSided) {
        for (int blend = 0; blend < 2; ++blend) {
            SDL_GPUVertexBufferDescription buffer{};
            buffer.slot = 0;
            buffer.pitch = kVertexFloats * sizeof(float);
            buffer.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
            SDL_GPUVertexAttribute attributes[4]{};
            attributes[0] = {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 0};
            attributes[1] = {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 3 * sizeof(float)};
            attributes[2] = {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 6 * sizeof(float)};
            attributes[3] = {3, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT, 8 * sizeof(float)};

            SDL_GPUColorTargetDescription color{};
            color.format = SDL_GetGPUSwapchainTextureFormat(device_, window_);
            if (blend) {
                color.blend_state.enable_blend = true;
                color.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
                color.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
                color.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
                color.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
                color.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
                color.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
            }

            SDL_GPUGraphicsPipelineCreateInfo info{};
            info.vertex_shader = vertex;
            info.fragment_shader = fragment;
            info.vertex_input_state.vertex_buffer_descriptions = &buffer;
            info.vertex_input_state.num_vertex_buffers = 1;
            info.vertex_input_state.vertex_attributes = attributes;
            info.vertex_input_state.num_vertex_attributes = 4;
            info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
            info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
            info.rasterizer_state.cull_mode = twoSided ? SDL_GPU_CULLMODE_NONE : SDL_GPU_CULLMODE_BACK;
            info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
            info.depth_stencil_state.enable_depth_test = true;
            info.depth_stencil_state.enable_depth_write = !blend;
            info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
            info.target_info.color_target_descriptions = &color;
            info.target_info.num_color_targets = 1;
            info.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
            info.target_info.has_depth_stencil_target = true;
            pipelines_[twoSided][blend] = SDL_CreateGPUGraphicsPipeline(device_, &info);
            if (pipelines_[twoSided][blend] == nullptr) {
                throw sdlError("pipeline");
            }
        }
    }
    SDL_ReleaseGPUShader(device_, vertex);
    SDL_ReleaseGPUShader(device_, fragment);

    // Sprites: blended over everything, without depth.
    {
        SDL_GPUShader* spriteVertex = shader("ui.vert.dxil", SDL_GPU_SHADERSTAGE_VERTEX, 0);
        SDL_GPUShader* spriteFragment = shader("ui.frag.dxil", SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 0);
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

    // Neighborhood terrain: opaque, both sides (as the game draws it).
    {
        SDL_GPUShader* terrainVertex = shader("terrain.vert.dxil", SDL_GPU_SHADERSTAGE_VERTEX, 0);
        SDL_GPUShader* terrainFragment = shader("terrain.frag.dxil", SDL_GPU_SHADERSTAGE_FRAGMENT, 5);
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
        info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
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
    {
        SDL_GPUShader* roadVertex = shader("road.vert.dxil", SDL_GPU_SHADERSTAGE_VERTEX, 0);
        SDL_GPUShader* roadFragment = shader("road.frag.dxil", SDL_GPU_SHADERSTAGE_FRAGMENT, 2);
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
        info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
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
    {
        SDL_GPUShader* imposterVertex = shader("imposter.vert.dxil", SDL_GPU_SHADERSTAGE_VERTEX, 0);
        SDL_GPUShader* imposterFragment = shader("imposter.frag.dxil", SDL_GPU_SHADERSTAGE_FRAGMENT, 1);
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
            info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
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
        // goes over; neither writes depth. Walls and slices: premultiplied, writing depth. Roofs:
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

    // The sky seen in the neighborhood water: behind everything, no depth.
    {
        SDL_GPUShader* skyVertex = shader("sky.vert.dxil", SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
        SDL_GPUShader* skyFragment = shader("sky.frag.dxil", SDL_GPU_SHADERSTAGE_FRAGMENT, 1);
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
    {
        SDL_GPUShader* waterVertex = shader("water.vert.dxil", SDL_GPU_SHADERSTAGE_VERTEX, 0);
        SDL_GPUShader* waterFragment = shader("water.frag.dxil", SDL_GPU_SHADERSTAGE_FRAGMENT, 2);
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
        info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
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
}

Renderer::~Renderer() {
    if (device_ == nullptr) {
        return;
    }
    release();
    releaseSprites();
    if (spriteVertices_) {
        SDL_ReleaseGPUBuffer(device_, spriteVertices_);
    }
    if (spritePipeline_) {
        SDL_ReleaseGPUGraphicsPipeline(device_, spritePipeline_);
    }
    for (auto* s : {clampedMips_, pointSampler_}) {
        if (s) {
            SDL_ReleaseGPUSampler(device_, s);
        }
    }
    for (auto* p : {terrainPipeline_, skyPipeline_, waterPipeline_, roadPipeline_, imposterLightPipeline_, imposterTerrainPipeline_,
                    imposterWallPipeline_, imposterRoofPipeline_}) {
        if (p) {
            SDL_ReleaseGPUGraphicsPipeline(device_, p);
        }
    }
    for (auto* t : {reflection_, reflectionDepth_}) {
        if (t) {
            SDL_ReleaseGPUTexture(device_, t);
        }
    }
    if (spriteSampler_) {
        SDL_ReleaseGPUSampler(device_, spriteSampler_);
    }
    for (auto& row : pipelines_) {
        for (auto* p : row) {
            if (p) {
                SDL_ReleaseGPUGraphicsPipeline(device_, p);
            }
        }
    }
    if (sampler_) {
        SDL_ReleaseGPUSampler(device_, sampler_);
    }
    if (depth_) {
        SDL_ReleaseGPUTexture(device_, depth_);
    }
    SDL_ReleaseWindowFromGPUDevice(device_, window_);
    SDL_DestroyGPUDevice(device_);
}

std::string Renderer::backend() const {
    const char* name = SDL_GetGPUDeviceDriver(device_);
    return name ? name : "";
}

void Renderer::release() {
    for (auto& mesh : meshes_) {
        for (auto& p : mesh.primitives) {
            SDL_ReleaseGPUBuffer(device_, p.vertices);
            SDL_ReleaseGPUBuffer(device_, p.indices);
        }
    }
    meshes_.clear();
    for (auto* t : textures_) {
        if (t) {
            SDL_ReleaseGPUTexture(device_, t);
        }
    }
    textures_.clear();
    if (white_) {
        SDL_ReleaseGPUTexture(device_, white_);
        white_ = nullptr;
    }
    for (auto** b : {&terrainVertices_, &terrainIndices_, &waterVertices_, &waterIndices_, &roadVertices_, &roadIndices_,
                     &imposterVertices_, &imposterIndices_}) {
        if (*b) {
            SDL_ReleaseGPUBuffer(device_, *b);
            *b = nullptr;
        }
    }
    for (auto** t : {&terrainPaints_, &terrainAlphas_, &terrainCells_, &terrainLight_, &terrainGradient_, &skyCube_, &ripples_, &roadTextures_}) {
        if (*t) {
            SDL_ReleaseGPUTexture(device_, *t);
            *t = nullptr;
        }
    }
    for (auto* t : imposterTextures_) {
        SDL_ReleaseGPUTexture(device_, t);
    }
    imposterTextures_.clear();
    imposterDraws_.clear();
    terrainIndexCount_ = 0;
    waterIndexCount_ = 0;
    roadIndexCount_ = 0;
}

void Renderer::releaseSprites() {
    for (auto* t : spriteTextures_) {
        if (t) {
            SDL_ReleaseGPUTexture(device_, t);
        }
    }
    spriteTextures_.clear();
    spriteSizes_.clear();
}

void Renderer::uploadSprites(const std::vector<scene::Image>& images) {
    releaseSprites();
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    std::vector<SDL_GPUTransferBuffer*> transfers;
    for (const auto& image : images) {
        if (image.width == 0 || image.height == 0 || image.rgba.size() != static_cast<std::size_t>(image.width) * image.height * 4) {
            spriteTextures_.push_back(nullptr);
            spriteSizes_.push_back({0, 0});
            continue;
        }
        SDL_GPUTextureCreateInfo info{};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        info.width = image.width;
        info.height = image.height;
        info.layer_count_or_depth = 1;
        info.num_levels = 1;
        SDL_GPUTexture* t = SDL_CreateGPUTexture(device_, &info);
        SDL_GPUTransferBufferCreateInfo tinfo{};
        tinfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        tinfo.size = static_cast<Uint32>(image.rgba.size());
        SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(device_, &tinfo);
        std::memcpy(SDL_MapGPUTransferBuffer(device_, tb, false), image.rgba.data(), image.rgba.size());
        SDL_UnmapGPUTransferBuffer(device_, tb);
        transfers.push_back(tb);
        SDL_GPUTextureTransferInfo from{};
        from.transfer_buffer = tb;
        from.pixels_per_row = image.width;
        from.rows_per_layer = image.height;
        SDL_GPUTextureRegion to{};
        to.texture = t;
        to.w = image.width;
        to.h = image.height;
        to.d = 1;
        SDL_UploadToGPUTexture(copy, &from, &to, false);
        spriteTextures_.push_back(t);
        spriteSizes_.push_back({image.width, image.height});
    }
    SDL_EndGPUCopyPass(copy);
    SDL_SubmitGPUCommandBuffer(cmd);
    for (auto* t : transfers) {
        SDL_ReleaseGPUTransferBuffer(device_, t);
    }
}

void Renderer::setSprites(const std::vector<std::pair<int, const scene::Image*>>& images) {
    if (images.empty()) {
        return;
    }
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    std::vector<SDL_GPUTransferBuffer*> transfers;
    for (const auto& [index, image] : images) {
        if (index < 0 || image->width == 0 || image->height == 0 ||
            image->rgba.size() != static_cast<std::size_t>(image->width) * image->height * 4) {
            continue;
        }
        const auto i = static_cast<std::size_t>(index);
        if (i >= spriteTextures_.size()) {
            spriteTextures_.resize(i + 1, nullptr);
            spriteSizes_.resize(i + 1, {0, 0});
        }
        // A new texture when the slot has none yet or the picture changed size.
        if (!spriteTextures_[i] || spriteSizes_[i][0] != image->width || spriteSizes_[i][1] != image->height) {
            if (spriteTextures_[i]) {
                SDL_ReleaseGPUTexture(device_, spriteTextures_[i]);
            }
            SDL_GPUTextureCreateInfo info{};
            info.type = SDL_GPU_TEXTURETYPE_2D;
            info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
            info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
            info.width = image->width;
            info.height = image->height;
            info.layer_count_or_depth = 1;
            info.num_levels = 1;
            spriteTextures_[i] = SDL_CreateGPUTexture(device_, &info);
            spriteSizes_[i] = {image->width, image->height};
        }
        SDL_GPUTransferBufferCreateInfo tinfo{};
        tinfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        tinfo.size = static_cast<Uint32>(image->rgba.size());
        SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(device_, &tinfo);
        std::memcpy(SDL_MapGPUTransferBuffer(device_, tb, false), image->rgba.data(), image->rgba.size());
        SDL_UnmapGPUTransferBuffer(device_, tb);
        transfers.push_back(tb);
        SDL_GPUTextureTransferInfo from{};
        from.transfer_buffer = tb;
        from.pixels_per_row = image->width;
        from.rows_per_layer = image->height;
        SDL_GPUTextureRegion to{};
        to.texture = spriteTextures_[i];
        to.w = image->width;
        to.h = image->height;
        to.d = 1;
        SDL_UploadToGPUTexture(copy, &from, &to, true);
    }
    SDL_EndGPUCopyPass(copy);
    SDL_SubmitGPUCommandBuffer(cmd);
    for (auto* t : transfers) {
        SDL_ReleaseGPUTransferBuffer(device_, t);
    }
}

void Renderer::showSprites(std::vector<Sprite> sprites, bool scene) {
    sprites_ = std::move(sprites);
    drawScene_ = scene;
}

void Renderer::uploadSpriteVertices(SDL_GPUCommandBuffer* cmd, unsigned, unsigned) {
    if (sprites_.empty()) {
        return;
    }
    std::vector<float> v;
    v.reserve(sprites_.size() * 6 * kSpriteFloats);
    for (const auto& s : sprites_) {
        const float corners[6][4] = {{s.x0, s.y0, s.u0, s.v0}, {s.x1, s.y0, s.u1, s.v0}, {s.x1, s.y1, s.u1, s.v1},
                                     {s.x0, s.y0, s.u0, s.v0}, {s.x1, s.y1, s.u1, s.v1}, {s.x0, s.y1, s.u0, s.v1}};
        for (const auto& c : corners) {
            v.insert(v.end(), {c[0], c[1], c[2], c[3], s.color[0], s.color[1], s.color[2], s.color[3]});
        }
    }
    const auto count = static_cast<unsigned>(v.size() / kSpriteFloats);
    if (count > spriteCapacity_) {
        if (spriteVertices_) {
            SDL_ReleaseGPUBuffer(device_, spriteVertices_);
        }
        spriteCapacity_ = std::max(count, spriteCapacity_ * 2);
        SDL_GPUBufferCreateInfo info{};
        info.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
        info.size = spriteCapacity_ * kSpriteFloats * static_cast<Uint32>(sizeof(float));
        spriteVertices_ = SDL_CreateGPUBuffer(device_, &info);
    }
    const auto bytes = static_cast<Uint32>(v.size() * sizeof(float));
    SDL_GPUTransferBufferCreateInfo tinfo{};
    tinfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tinfo.size = bytes;
    SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(device_, &tinfo);
    std::memcpy(SDL_MapGPUTransferBuffer(device_, tb, false), v.data(), bytes);
    SDL_UnmapGPUTransferBuffer(device_, tb);
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTransferBufferLocation from{tb, 0};
    SDL_GPUBufferRegion to{spriteVertices_, 0, bytes};
    SDL_UploadToGPUBuffer(copy, &from, &to, false);
    SDL_EndGPUCopyPass(copy);
    SDL_ReleaseGPUTransferBuffer(device_, tb);
}

void Renderer::upload(const scene::Scene& scene) {
    release();
    materials_ = scene.materials;
    instances_ = scene.instances;
    visible_.clear();

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    std::vector<SDL_GPUTransferBuffer*> transfers;
    auto stage = [&](const void* data, Uint32 size) {
        SDL_GPUTransferBufferCreateInfo info{};
        info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        info.size = size;
        SDL_GPUTransferBuffer* t = SDL_CreateGPUTransferBuffer(device_, &info);
        void* mapped = SDL_MapGPUTransferBuffer(device_, t, false);
        std::memcpy(mapped, data, size);
        SDL_UnmapGPUTransferBuffer(device_, t);
        transfers.push_back(t);
        return t;
    };
    auto buffer = [&](SDL_GPUBufferUsageFlags usage, const void* data, Uint32 size) {
        SDL_GPUBufferCreateInfo info{};
        info.usage = usage;
        info.size = size;
        SDL_GPUBuffer* b = SDL_CreateGPUBuffer(device_, &info);
        SDL_GPUTransferBufferLocation from{stage(data, size), 0};
        SDL_GPUBufferRegion to{b, 0, size};
        SDL_UploadToGPUBuffer(copy, &from, &to, false);
        return b;
    };
    std::vector<SDL_GPUTexture*> withMips;
    auto texture = [&](const scene::Image& image) {
        const Uint32 levels = static_cast<Uint32>(std::floor(std::log2(std::max(image.width, image.height)))) + 1;
        SDL_GPUTextureCreateInfo info{};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
        info.width = image.width;
        info.height = image.height;
        info.layer_count_or_depth = 1;
        info.num_levels = levels;
        SDL_GPUTexture* t = SDL_CreateGPUTexture(device_, &info);
        SDL_GPUTextureTransferInfo from{};
        from.transfer_buffer = stage(image.rgba.data(), static_cast<Uint32>(image.rgba.size()));
        from.pixels_per_row = image.width;
        from.rows_per_layer = image.height;
        SDL_GPUTextureRegion to{};
        to.texture = t;
        to.w = image.width;
        to.h = image.height;
        to.d = 1;
        SDL_UploadToGPUTexture(copy, &from, &to, false);
        if (levels > 1) {
            withMips.push_back(t);
        }
        return t;
    };

    for (const auto& image : scene.images) {
        textures_.push_back(image.width && image.height && image.rgba.size() == static_cast<std::size_t>(image.width) * image.height * 4
                                ? texture(image)
                                : nullptr);
    }
    white_ = texture({1, 1, {255, 255, 255, 255}});

    std::vector<float> vertices;
    for (const auto& mesh : scene.meshes) {
        GpuMesh gm;
        for (const auto& p : mesh.primitives) {
            const std::size_t n = p.positions.size() / 3;
            if (n == 0 || p.indices.empty()) {
                continue;
            }
            vertices.assign(n * kVertexFloats, 0.0f);
            for (std::size_t i = 0; i < n; ++i) {
                float* v = &vertices[i * kVertexFloats];
                std::copy_n(&p.positions[i * 3], 3, v);
                if (p.normals.size() == p.positions.size()) {
                    std::copy_n(&p.normals[i * 3], 3, v + 3);
                } else {
                    v[5] = 1;
                }
                if (p.uvs.size() == n * 2) {
                    std::copy_n(&p.uvs[i * 2], 2, v + 6);
                }
                v[8] = p.alphas.size() == n ? p.alphas[i] : 1.0f;
            }
            GpuPrimitive g;
            g.vertices = buffer(SDL_GPU_BUFFERUSAGE_VERTEX, vertices.data(), static_cast<Uint32>(vertices.size() * sizeof(float)));
            g.indices = buffer(SDL_GPU_BUFFERUSAGE_INDEX, p.indices.data(), static_cast<Uint32>(p.indices.size() * sizeof(std::uint32_t)));
            g.count = static_cast<unsigned>(p.indices.size());
            g.material = p.material;
            gm.primitives.push_back(g);
        }
        meshes_.push_back(std::move(gm));
    }
    if (scene.terrain && scene.terrain->paints() > 0 && !scene.terrain->textures.empty()) {
        const auto& t = *scene.terrain;
        // Vertices: position, normal, grid position.
        std::vector<float> v(static_cast<std::size_t>(t.columns) * t.rows * 8);
        for (std::uint32_t y = 0; y < t.rows; ++y) {
            for (std::uint32_t x = 0; x < t.columns; ++x) {
                const std::size_t i = static_cast<std::size_t>(y) * t.columns + x;
                float* p = &v[i * 8];
                p[0] = static_cast<float>(x) * t.tileSize;
                p[1] = static_cast<float>(y) * t.tileSize;
                p[2] = t.heights[i];
                std::copy_n(&t.normals[i * 3], 3, p + 3);
                p[6] = static_cast<float>(x);
                p[7] = static_cast<float>(y);
            }
        }
        // Two triangles per tile, split from its (x, y) corner to its (x + 1, y + 1) corner.
        std::vector<std::uint32_t> idx;
        idx.reserve(static_cast<std::size_t>(t.columns - 1) * (t.rows - 1) * 6);
        for (std::uint32_t y = 0; y + 1 < t.rows; ++y) {
            for (std::uint32_t x = 0; x + 1 < t.columns; ++x) {
                const std::uint32_t a = y * t.columns + x, b = a + 1, d = a + t.columns, e = d + 1;
                idx.insert(idx.end(), {a, b, e, a, e, d});
            }
        }
        terrainVertices_ = buffer(SDL_GPU_BUFFERUSAGE_VERTEX, v.data(), static_cast<Uint32>(v.size() * sizeof(float)));
        terrainIndices_ = buffer(SDL_GPU_BUFFERUSAGE_INDEX, idx.data(), static_cast<Uint32>(idx.size() * sizeof(std::uint32_t)));
        terrainIndexCount_ = static_cast<unsigned>(idx.size());

        auto array = [&](SDL_GPUTextureFormat format, Uint32 w, Uint32 h, Uint32 layers, Uint32 levels, Uint32 bytesPerTexel,
                         auto layerData) {
            SDL_GPUTextureCreateInfo info{};
            info.type = SDL_GPU_TEXTURETYPE_2D_ARRAY;
            info.format = format;
            info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | (levels > 1 ? SDL_GPU_TEXTUREUSAGE_COLOR_TARGET : 0);
            info.width = w;
            info.height = h;
            info.layer_count_or_depth = layers;
            info.num_levels = levels;
            SDL_GPUTexture* tex = SDL_CreateGPUTexture(device_, &info);
            for (Uint32 l = 0; l < layers; ++l) {
                SDL_GPUTextureTransferInfo from{};
                from.transfer_buffer = stage(layerData(l), w * h * bytesPerTexel);
                from.pixels_per_row = w;
                from.rows_per_layer = h;
                SDL_GPUTextureRegion to{};
                to.texture = tex;
                to.layer = l;
                to.w = w;
                to.h = h;
                to.d = 1;
                SDL_UploadToGPUTexture(copy, &from, &to, false);
            }
            return tex;
        };
        const auto& first = t.textures.front();
        const Uint32 levels = static_cast<Uint32>(std::floor(std::log2(std::max(first.width, first.height)))) + 1;
        terrainPaints_ = array(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, first.width, first.height, static_cast<Uint32>(t.textures.size()),
                               levels, 4, [&](Uint32 l) { return t.textures[l].rgba.data(); });
        if (levels > 1) {
            withMips.push_back(terrainPaints_);
        }
        const Uint32 paints = static_cast<Uint32>(t.paints());
        const std::size_t map = static_cast<std::size_t>(t.alphaSize) * t.alphaSize;
        terrainAlphas_ = array(SDL_GPU_TEXTUREFORMAT_R8_UNORM, t.alphaSize, t.alphaSize, paints, 1, 1,
                               [&](Uint32 l) { return t.alphas.data() + l * map; });
        const std::size_t cells = static_cast<std::size_t>(t.cellsX) * t.cellsY;
        terrainCells_ = array(SDL_GPU_TEXTUREFORMAT_R8_UNORM, t.cellsX, t.cellsY, paints, 1, 1,
                              [&](Uint32 l) { return t.cellTextures.data() + l * cells; });
        // The light map; white (no change) when there is none.
        const scene::Image white{1, 1, {255, 255, 255, 255}};
        const scene::Image& light = t.lightMap.width ? t.lightMap : white;
        terrainLight_ = array(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, light.width, light.height, 1, 1, 4,
                              [&](Uint32) { return light.rgba.data(); });
        terrainUniforms_ = {static_cast<float>(t.alphaSize), t.textureRepeat, static_cast<float>(t.cellTiles), static_cast<float>(paints),
                            static_cast<float>(t.cellsX), static_cast<float>(t.cellsY), t.lightMap.width ? t.lightMapScale : 1.0f, t.litFrom,
                            t.gradientDepth, 0, {0, 0}};

        // A plain 2D texture, with or without mip levels.
        auto plain = [&](const scene::Image& image, bool mips) {
            const Uint32 levels = mips ? static_cast<Uint32>(std::floor(std::log2(std::max(image.width, image.height)))) + 1 : 1;
            SDL_GPUTextureCreateInfo info{};
            info.type = SDL_GPU_TEXTURETYPE_2D;
            info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
            info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | (levels > 1 ? SDL_GPU_TEXTUREUSAGE_COLOR_TARGET : 0);
            info.width = image.width;
            info.height = image.height;
            info.layer_count_or_depth = 1;
            info.num_levels = levels;
            SDL_GPUTexture* tex = SDL_CreateGPUTexture(device_, &info);
            SDL_GPUTextureTransferInfo from{};
            from.transfer_buffer = stage(image.rgba.data(), static_cast<Uint32>(image.rgba.size()));
            from.pixels_per_row = image.width;
            from.rows_per_layer = image.height;
            SDL_GPUTextureRegion to{};
            to.texture = tex;
            to.w = image.width;
            to.h = image.height;
            to.d = 1;
            SDL_UploadToGPUTexture(copy, &from, &to, false);
            if (levels > 1) {
                withMips.push_back(tex);
            }
            return tex;
        };
        terrainGradient_ = plain(t.underwaterGradient.width ? t.underwaterGradient : white, false);

        // The water sheet: a quad per wet tile at the water level, texture coordinates in tiles.
        std::vector<float> wv;
        std::vector<std::uint32_t> wi;
        const std::uint32_t tilesX = t.columns - 1;
        for (std::size_t i = 0; i < t.waterTiles.size(); ++i) {
            if (!t.waterTiles[i]) {
                continue;
            }
            const auto x = static_cast<float>(i % tilesX), y = static_cast<float>(i / tilesX);
            const auto base = static_cast<std::uint32_t>(wv.size() / 5);
            for (const auto [cx, cy] : {std::pair{x, y}, std::pair{x + 1, y}, std::pair{x + 1, y + 1}, std::pair{x, y + 1}}) {
                wv.insert(wv.end(), {cx * t.tileSize, cy * t.tileSize, t.waterLevel, cx, cy});
            }
            wi.insert(wi.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
        }
        if (!wi.empty() && t.sky.size() == 6 && t.ripples.width) {
            waterVertices_ = buffer(SDL_GPU_BUFFERUSAGE_VERTEX, wv.data(), static_cast<Uint32>(wv.size() * sizeof(float)));
            waterIndices_ = buffer(SDL_GPU_BUFFERUSAGE_INDEX, wi.data(), static_cast<Uint32>(wi.size() * sizeof(std::uint32_t)));
            waterIndexCount_ = static_cast<unsigned>(wi.size());
            waterLevel_ = t.waterLevel;
            ripples_ = plain(t.ripples, true);
            // The sky cube map: six faces of one size.
            const auto& face = t.sky.front();
            const Uint32 skyLevels = static_cast<Uint32>(std::floor(std::log2(std::max(face.width, face.height)))) + 1;
            SDL_GPUTextureCreateInfo info{};
            info.type = SDL_GPU_TEXTURETYPE_CUBE;
            info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
            info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
            info.width = face.width;
            info.height = face.height;
            info.layer_count_or_depth = 6;
            info.num_levels = skyLevels;
            skyCube_ = SDL_CreateGPUTexture(device_, &info);
            for (Uint32 l = 0; l < 6; ++l) {
                SDL_GPUTextureTransferInfo from{};
                from.transfer_buffer = stage(t.sky[l].rgba.data(), static_cast<Uint32>(t.sky[l].rgba.size()));
                from.pixels_per_row = face.width;
                from.rows_per_layer = face.height;
                SDL_GPUTextureRegion to{};
                to.texture = skyCube_;
                to.layer = l;
                to.w = face.width;
                to.h = face.height;
                to.d = 1;
                SDL_UploadToGPUTexture(copy, &from, &to, false);
            }
            withMips.push_back(skyCube_);
            waterUniforms_ = {{t.reflectionTint[0], t.reflectionTint[1], t.reflectionTint[2], t.reflectionStrength}, {0, 0},
                              t.reflectionOffset, t.rippleScale, t.rippleSpeed, 0, {0, 0}};
        }

        // The roads: a quad per tile, split like the terrain's tiles.
        if (!t.roads.empty() && !t.roadTextures.empty()) {
            std::vector<float> rv;
            std::vector<std::uint32_t> ri;
            rv.reserve(t.roads.size() * 24);
            ri.reserve(t.roads.size() * 6);
            for (const auto& road : t.roads) {
                const auto base = static_cast<std::uint32_t>(rv.size() / 6);
                for (const auto& c : road.corners) {
                    rv.insert(rv.end(), {c[0], c[1], c[2], c[3], c[4], static_cast<float>(road.texture)});
                }
                ri.insert(ri.end(), {base, base + 2, base + 1, base, base + 3, base + 2});
            }
            roadVertices_ = buffer(SDL_GPU_BUFFERUSAGE_VERTEX, rv.data(), static_cast<Uint32>(rv.size() * sizeof(float)));
            roadIndices_ = buffer(SDL_GPU_BUFFERUSAGE_INDEX, ri.data(), static_cast<Uint32>(ri.size() * sizeof(std::uint32_t)));
            roadIndexCount_ = static_cast<unsigned>(ri.size());
            const auto& road = t.roadTextures.front();
            const Uint32 roadLevels = static_cast<Uint32>(std::floor(std::log2(std::max(road.width, road.height)))) + 1;
            roadTextures_ = array(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, road.width, road.height, static_cast<Uint32>(t.roadTextures.size()),
                                  roadLevels, 4, [&](Uint32 l) { return t.roadTextures[l].rgba.data(); });
            if (roadLevels > 1) {
                withMips.push_back(roadTextures_);
            }
            lightMapSize_ = t.lightMapScale * t.tileSize;
        }

        // The lot imposters: all parts in one buffer, each drawn with its own texture.
        if (!t.imposters.empty()) {
            for (const auto& image : t.imposterTextures) {
                imposterTextures_.push_back(plain(image, false));
            }
            std::vector<float> iv;
            std::vector<std::uint32_t> ii;
            for (const auto& part : t.imposters) {
                ImposterDraw draw{part.kind};
                draw.texture = imposterTextures_[part.texture];
                draw.lightMap = part.lightMap >= 0 ? imposterTextures_[static_cast<std::size_t>(part.lightMap)] : nullptr;
                std::copy_n(part.lightMapTint.data(), 3, draw.tint);
                draw.firstIndex = static_cast<std::uint32_t>(ii.size());
                draw.indexCount = static_cast<std::uint32_t>(part.indices.size());
                draw.vertexOffset = static_cast<std::int32_t>(iv.size() / 7);
                iv.insert(iv.end(), part.vertices.begin(), part.vertices.end());
                ii.insert(ii.end(), part.indices.begin(), part.indices.end());
                imposterDraws_.push_back(draw);
            }
            imposterVertices_ = buffer(SDL_GPU_BUFFERUSAGE_VERTEX, iv.data(), static_cast<Uint32>(iv.size() * sizeof(float)));
            imposterIndices_ = buffer(SDL_GPU_BUFFERUSAGE_INDEX, ii.data(), static_cast<Uint32>(ii.size() * sizeof(std::uint32_t)));
        }
    }

    SDL_EndGPUCopyPass(copy);
    for (auto* t : withMips) {
        SDL_GenerateMipmapsForGPUTexture(cmd, t);
    }
    SDL_SubmitGPUCommandBuffer(cmd);
    for (auto* t : transfers) {
        SDL_ReleaseGPUTransferBuffer(device_, t);
    }
}

void Renderer::drawImposters(SDL_GPUCommandBuffer* cmd, SDL_GPURenderPass* pass, const Mat4& viewProjection) {
    if (imposterDraws_.empty()) {
        return;
    }
    using Kind = scene::PaintedTerrain::ImposterPart::Kind;
    SDL_GPUBufferBinding vertices{imposterVertices_, 0};
    SDL_GPUBufferBinding indices{imposterIndices_, 0};
    struct PartUniforms {
        float tint[3];
        float cutoff, mode, padding[3];
    };
    auto draw = [&](SDL_GPUGraphicsPipeline* pipeline, const ImposterDraw& d, SDL_GPUTexture* texture, SDL_GPUSampler* sampler,
                    float cutoff, float mode) {
        SDL_BindGPUGraphicsPipeline(pass, pipeline);
        SDL_PushGPUVertexUniformData(cmd, 0, viewProjection.m.data(), 16 * sizeof(float));
        const PartUniforms u{{d.tint[0], d.tint[1], d.tint[2]}, cutoff, mode, {0, 0, 0}};
        SDL_PushGPUFragmentUniformData(cmd, 0, &u, sizeof(u));
        SDL_GPUTextureSamplerBinding binding{texture, sampler};
        SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
        SDL_BindGPUVertexBuffers(pass, 0, &vertices, 1);
        SDL_BindGPUIndexBuffer(pass, &indices, SDL_GPU_INDEXELEMENTSIZE_32BIT);
        SDL_DrawGPUIndexedPrimitives(pass, d.indexCount, 1, d.firstIndex, d.vertexOffset, 0);
    };
    // The terrain first (its shader layer comes before the others'): light map, then texture.
    for (const auto& d : imposterDraws_) {
        if (d.kind != Kind::Terrain) {
            continue;
        }
        if (d.lightMap) {
            draw(imposterLightPipeline_, d, d.lightMap, sampler_, 0, 0);
        }
        draw(imposterTerrainPipeline_, d, d.texture, clampedMips_, 0, 1);
    }
    for (const auto& d : imposterDraws_) {
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
    SDL_GPUTexture* target = nullptr;
    Uint32 width = 0, height = 0;
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd, window_, &target, &width, &height) || target == nullptr) {
        SDL_SubmitGPUCommandBuffer(cmd);
        return false;
    }
    render(cmd, target, width, height, camera);
    SDL_SubmitGPUCommandBuffer(cmd);
    return true;
}

bool Renderer::drawToFile(const Camera& camera, unsigned width, unsigned height, const std::filesystem::path& path) {
    const SDL_GPUTextureFormat format = SDL_GetGPUSwapchainTextureFormat(device_, window_);
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

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    render(cmd, target, width, height, camera);
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
    SDL_WaitForGPUFences(device_, true, &fence, 1);
    SDL_ReleaseGPUFence(device_, fence);

    void* pixels = SDL_MapGPUTransferBuffer(device_, transfer, false);
    const SDL_PixelFormat pixelFormat =
        format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM ? SDL_PIXELFORMAT_BGRA32 : SDL_PIXELFORMAT_RGBA32;
    SDL_Surface* surface = SDL_CreateSurfaceFrom(static_cast<int>(width), static_cast<int>(height), pixelFormat, pixels,
                                                 static_cast<int>(width * 4));
    const bool saved = surface != nullptr && SDL_SavePNG(surface, path.string().c_str());
    SDL_DestroySurface(surface);
    SDL_UnmapGPUTransferBuffer(device_, transfer);
    SDL_ReleaseGPUTransferBuffer(device_, transfer);
    SDL_ReleaseGPUTexture(device_, target);
    return saved;
}

void Renderer::render(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* target, unsigned width, unsigned height,
                      const Camera& camera) {
    if (depth_ == nullptr || depthWidth_ != width || depthHeight_ != height) {
        if (depth_) {
            SDL_ReleaseGPUTexture(device_, depth_);
        }
        SDL_GPUTextureCreateInfo info{};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
        info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
        info.width = width;
        info.height = height;
        info.layer_count_or_depth = 1;
        info.num_levels = 1;
        depth_ = SDL_CreateGPUTexture(device_, &info);
        depthWidth_ = width;
        depthHeight_ = height;
    }

    uploadSpriteVertices(cmd, width, height);

    const float aspect = height ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;
    const float fovY = camera.fovX > 0 ? 2 * std::atan(std::tan(camera.fovX / 2) / aspect) : camera.fovY;
    const Mat4 viewProjection = Mat4::perspective(fovY, aspect, camera.nearPlane > 0 ? camera.nearPlane : camera.farPlane / 10000, camera.farPlane) *
                                Mat4::lookAt(camera.eye, camera.target, {0, 0, 1});

    auto drawTerrain = [&](SDL_GPURenderPass* on, const Mat4& vp, bool reflected) {
        SDL_BindGPUGraphicsPipeline(on, terrainPipeline_);
        SDL_PushGPUVertexUniformData(cmd, 0, vp.m.data(), 16 * sizeof(float));
        TerrainUniforms u = terrainUniforms_;
        u.reflected = reflected ? 1.0f : 0.0f;
        SDL_PushGPUFragmentUniformData(cmd, 0, &u, sizeof(u));
        // The light map wraps: the game's lighting stage sets no addressing, leaving D3D's default.
        SDL_GPUTextureSamplerBinding bindings[5] = {{terrainPaints_, sampler_},      {terrainAlphas_, spriteSampler_},
                                                    {terrainCells_, spriteSampler_}, {terrainLight_, sampler_},
                                                    {terrainGradient_, spriteSampler_}};
        SDL_BindGPUFragmentSamplers(on, 0, bindings, 5);
        SDL_GPUBufferBinding vertices{terrainVertices_, 0};
        SDL_BindGPUVertexBuffers(on, 0, &vertices, 1);
        SDL_GPUBufferBinding indices{terrainIndices_, 0};
        SDL_BindGPUIndexBuffer(on, &indices, SDL_GPU_INDEXELEMENTSIZE_32BIT);
        SDL_DrawGPUIndexedPrimitives(on, terrainIndexCount_, 1, 0, 0, 0);
    };

    // The neighborhood water's reflection: the sky and the terrain above the water, mirrored in it.
    const bool water = drawScene_ && terrainIndexCount_ > 0 && waterIndexCount_ > 0;
    if (water) {
        if (reflection_ == nullptr || reflectionWidth_ != width || reflectionHeight_ != height) {
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
            reflectionWidth_ = width;
            reflectionHeight_ = height;
        }
        SDL_GPUColorTargetInfo rc{};
        rc.texture = reflection_;
        rc.load_op = SDL_GPU_LOADOP_CLEAR;
        rc.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPUDepthStencilTargetInfo rd{};
        rd.texture = reflectionDepth_;
        rd.clear_depth = 1;
        rd.load_op = SDL_GPU_LOADOP_CLEAR;
        rd.store_op = SDL_GPU_STOREOP_DONT_CARE;
        rd.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
        rd.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
        SDL_GPURenderPass* mirror = SDL_BeginGPURenderPass(cmd, &rc, 1, &rd);
        // The sky, by each pixel's ray mirrored in the water.
        const Vec3 f = normalize(sub(camera.target, camera.eye));
        const Vec3 s = normalize(cross(f, {0, 0, 1}));
        const Vec3 u = cross(s, f);
        const float ty = std::tan(fovY / 2), tx = ty * aspect;
        const float skyCamera[12] = {s[0] * tx, s[1] * tx, s[2] * tx, 0, u[0] * ty, u[1] * ty, u[2] * ty, 0, f[0], f[1], f[2], 0};
        SDL_BindGPUGraphicsPipeline(mirror, skyPipeline_);
        SDL_PushGPUFragmentUniformData(cmd, 0, skyCamera, sizeof(skyCamera));
        SDL_GPUTextureSamplerBinding skyBinding{skyCube_, sampler_};
        SDL_BindGPUFragmentSamplers(mirror, 0, &skyBinding, 1);
        SDL_DrawGPUPrimitives(mirror, 3, 1, 0, 0);
        // The terrain, mirrored about the water's surface.
        Mat4 flip;
        flip.m = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1, 2 * waterLevel_, 0, 0, 0, 1};
        drawTerrain(mirror, viewProjection * flip, true);
        SDL_EndGPURenderPass(mirror);
    }

    SDL_GPUColorTargetInfo color{};
    color.texture = target;
    color.clear_color = {clear_[0], clear_[1], clear_[2], 1.0f};
    color.load_op = SDL_GPU_LOADOP_CLEAR;
    color.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPUDepthStencilTargetInfo depth{};
    depth.texture = depth_;
    depth.clear_depth = 1;
    depth.load_op = SDL_GPU_LOADOP_CLEAR;
    depth.store_op = SDL_GPU_STOREOP_DONT_CARE;
    depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &color, 1, &depth);

    const Vec3 sun = normalize({0.4f, -0.5f, 0.75f});

    struct Draw {
        const GpuPrimitive* primitive;
        const scene::Instance* instance;
        float distance;
    };
    std::vector<Draw> opaque, layers, blended;
    for (std::size_t i = 0; drawScene_ && i < instances_.size(); ++i) {
        const auto& instance = instances_[i];
        if (instance.mesh < 0 || static_cast<std::size_t>(instance.mesh) >= meshes_.size() || (i < visible_.size() && !visible_[i])) {
            continue;
        }
        const Vec3 at{instance.transform.m[3], instance.transform.m[7], instance.transform.m[11]};
        const Vec3 d = sub(at, camera.eye);
        for (const auto& p : meshes_[static_cast<std::size_t>(instance.mesh)].primitives) {
            const auto alpha = p.material >= 0 ? materials_[static_cast<std::size_t>(p.material)].alpha : scene::AlphaMode::Opaque;
            (alpha == scene::AlphaMode::Blend ? blended : alpha == scene::AlphaMode::Layer ? layers : opaque)
                .push_back({&p, &instance, dot(d, d)});
        }
    }
    // Blended surfaces from the farthest to the nearest.
    std::sort(blended.begin(), blended.end(), [](const Draw& a, const Draw& b) { return a.distance > b.distance; });

    auto drawList = [&](const std::vector<Draw>& list, int blend) {
        SDL_GPUGraphicsPipeline* bound = nullptr;
        for (const auto& d : list) {
            const scene::Material* m = d.primitive->material >= 0 ? &materials_[static_cast<std::size_t>(d.primitive->material)] : nullptr;
            SDL_GPUGraphicsPipeline* pipeline = pipelines_[m && m->doubleSided ? 1 : 0][blend];
            if (pipeline != bound) {
                SDL_BindGPUGraphicsPipeline(pass, pipeline);
                bound = pipeline;
            }
            VertexUniforms vu{};
            std::copy(viewProjection.m.begin(), viewProjection.m.end(), vu.viewProjection);
            const auto& t = d.instance->transform.m;
            const float model[16] = {t[0], t[1], t[2], t[3], t[4], t[5], t[6], t[7], t[8], t[9], t[10], t[11], 0, 0, 0, 1};
            std::copy(std::begin(model), std::end(model), vu.model);
            SDL_PushGPUVertexUniformData(cmd, 0, &vu, sizeof(vu));

            FragmentUniforms fu{};
            const auto c = m ? m->color : std::array<float, 4>{0.8f, 0.8f, 0.8f, 1};
            std::copy(c.begin(), c.end(), fu.color);
            fu.sunDirection[0] = sun[0];
            fu.sunDirection[1] = sun[1];
            fu.sunDirection[2] = sun[2];
            SDL_GPUTexture* texture = white_;
            if (m && m->image >= 0 && static_cast<std::size_t>(m->image) < textures_.size() && textures_[static_cast<std::size_t>(m->image)]) {
                texture = textures_[static_cast<std::size_t>(m->image)];
                fu.hasTexture = 1;
            }
            fu.alphaCutoff = !m                                     ? 0.0f
                             : m->alpha == scene::AlphaMode::Mask  ? m->alphaReference
                             : m->alpha == scene::AlphaMode::Layer ? 10.0f / 255  // as the game's terrain paint
                             : blend                               ? 0.01f
                                                                   : 0.0f;
            fu.unlit = m && m->unlit ? 1.0f : 0.0f;
            SDL_PushGPUFragmentUniformData(cmd, 0, &fu, sizeof(fu));

            SDL_GPUTextureSamplerBinding binding{texture, sampler_};
            SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
            SDL_GPUBufferBinding vertices{d.primitive->vertices, 0};
            SDL_BindGPUVertexBuffers(pass, 0, &vertices, 1);
            SDL_GPUBufferBinding indices{d.primitive->indices, 0};
            SDL_BindGPUIndexBuffer(pass, &indices, SDL_GPU_INDEXELEMENTSIZE_32BIT);
            SDL_DrawGPUIndexedPrimitives(pass, d.primitive->count, 1, 0, 0, 0);
        }
    };
    drawList(opaque, 0);
    if (drawScene_ && terrainIndexCount_ > 0) {
        drawTerrain(pass, viewProjection, false);
        if (roadIndexCount_ > 0) {
            SDL_BindGPUGraphicsPipeline(pass, roadPipeline_);
            SDL_PushGPUVertexUniformData(cmd, 0, viewProjection.m.data(), 16 * sizeof(float));
            const float road[4] = {lightMapSize_, 0, 0, 0};
            SDL_PushGPUFragmentUniformData(cmd, 0, road, sizeof(road));
            SDL_GPUTextureSamplerBinding bindings[2] = {{roadTextures_, clampedMips_}, {terrainLight_, sampler_}};
            SDL_BindGPUFragmentSamplers(pass, 0, bindings, 2);
            SDL_GPUBufferBinding vertices{roadVertices_, 0};
            SDL_BindGPUVertexBuffers(pass, 0, &vertices, 1);
            SDL_GPUBufferBinding indices{roadIndices_, 0};
            SDL_BindGPUIndexBuffer(pass, &indices, SDL_GPU_INDEXELEMENTSIZE_32BIT);
            SDL_DrawGPUIndexedPrimitives(pass, roadIndexCount_, 1, 0, 0, 0);
        }
        drawImposters(cmd, pass, viewProjection);
    }
    drawList(layers, 1);
    if (water) {
        SDL_BindGPUGraphicsPipeline(pass, waterPipeline_);
        SDL_PushGPUVertexUniformData(cmd, 0, viewProjection.m.data(), 16 * sizeof(float));
        WaterUniforms w = waterUniforms_;
        w.screen[0] = static_cast<float>(width);
        w.screen[1] = static_cast<float>(height);
        w.time = time_;
        SDL_PushGPUFragmentUniformData(cmd, 0, &w, sizeof(w));
        SDL_GPUTextureSamplerBinding bindings[2] = {{reflection_, spriteSampler_}, {ripples_, sampler_}};
        SDL_BindGPUFragmentSamplers(pass, 0, bindings, 2);
        SDL_GPUBufferBinding vertices{waterVertices_, 0};
        SDL_BindGPUVertexBuffers(pass, 0, &vertices, 1);
        SDL_GPUBufferBinding indices{waterIndices_, 0};
        SDL_BindGPUIndexBuffer(pass, &indices, SDL_GPU_INDEXELEMENTSIZE_32BIT);
        SDL_DrawGPUIndexedPrimitives(pass, waterIndexCount_, 1, 0, 0, 0);
    }
    drawList(blended, 1);

    // Sprites in order, one draw per run of the same texture.
    if (!sprites_.empty() && spriteVertices_) {
        SDL_BindGPUGraphicsPipeline(pass, spritePipeline_);
        const float screen[4] = {static_cast<float>(width), static_cast<float>(height), 0, 0};
        SDL_PushGPUVertexUniformData(cmd, 0, screen, sizeof(screen));
        SDL_GPUBufferBinding vertices{spriteVertices_, 0};
        SDL_BindGPUVertexBuffers(pass, 0, &vertices, 1);
        for (std::size_t i = 0; i < sprites_.size();) {
            const int t = sprites_[i].texture;
            std::size_t j = i;
            while (j < sprites_.size() && sprites_[j].texture == t) {
                ++j;
            }
            const bool has = t >= 0 && static_cast<std::size_t>(t) < spriteTextures_.size() && spriteTextures_[static_cast<std::size_t>(t)];
            if (!has && !white_) {
                i = j;
                continue;
            }
            SDL_GPUTextureSamplerBinding binding{has ? spriteTextures_[static_cast<std::size_t>(t)] : white_, spriteSampler_};
            SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
            SDL_DrawGPUPrimitives(pass, static_cast<Uint32>((j - i) * 6), 1, static_cast<Uint32>(i * 6), 0);
            i = j;
        }
    }
    SDL_EndGPURenderPass(pass);
}

} // namespace ts2::render
