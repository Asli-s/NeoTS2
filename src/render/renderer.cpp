#include "render/renderer.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace ts2::render {

namespace {

constexpr std::uint32_t kVertexFloats = 8;  // position, normal, texture coordinates

struct VertexUniforms {
    float viewProjection[16];
    float model[16];
};

struct FragmentUniforms {
    float color[4];
    float sunDirection[4];
    float hasTexture;
    float alphaCutoff;
    float padding[2];
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

    auto shader = [&](const char* file, SDL_GPUShaderStage stage, Uint32 samplers) {
        const auto code = readFile(shaderFolder / file);
        SDL_GPUShaderCreateInfo info{};
        info.code_size = code.size();
        info.code = code.data();
        info.entrypoint = "main";
        info.format = SDL_GPU_SHADERFORMAT_DXIL;
        info.stage = stage;
        info.num_samplers = samplers;
        info.num_uniform_buffers = 1;
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
            SDL_GPUVertexAttribute attributes[3]{};
            attributes[0] = {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 0};
            attributes[1] = {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 3 * sizeof(float)};
            attributes[2] = {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 6 * sizeof(float)};

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
            info.vertex_input_state.num_vertex_attributes = 3;
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
}

Renderer::~Renderer() {
    if (device_ == nullptr) {
        return;
    }
    release();
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
}

void Renderer::upload(const scene::Scene& scene) {
    release();
    materials_ = scene.materials;
    instances_ = scene.instances;

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
    SDL_EndGPUCopyPass(copy);
    for (auto* t : withMips) {
        SDL_GenerateMipmapsForGPUTexture(cmd, t);
    }
    SDL_SubmitGPUCommandBuffer(cmd);
    for (auto* t : transfers) {
        SDL_ReleaseGPUTransferBuffer(device_, t);
    }
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

    SDL_GPUColorTargetInfo color{};
    color.texture = target;
    color.clear_color = {0.17f, 0.18f, 0.19f, 1.0f};
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

    const float aspect = height ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;
    const Mat4 viewProjection = Mat4::perspective(camera.fovY, aspect, 0.05f, 500.0f) * Mat4::lookAt(camera.eye, camera.target, {0, 0, 1});
    const Vec3 sun = normalize({0.4f, -0.5f, 0.75f});

    struct Draw {
        const GpuPrimitive* primitive;
        const scene::Instance* instance;
        float distance;
    };
    std::vector<Draw> opaque, blended;
    for (const auto& instance : instances_) {
        if (instance.mesh < 0 || static_cast<std::size_t>(instance.mesh) >= meshes_.size()) {
            continue;
        }
        const Vec3 at{instance.transform.m[3], instance.transform.m[7], instance.transform.m[11]};
        const Vec3 d = sub(at, camera.eye);
        for (const auto& p : meshes_[static_cast<std::size_t>(instance.mesh)].primitives) {
            const bool blend = p.material >= 0 && materials_[static_cast<std::size_t>(p.material)].alpha == scene::AlphaMode::Blend;
            (blend ? blended : opaque).push_back({&p, &instance, dot(d, d)});
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
            fu.alphaCutoff = m && m->alpha == scene::AlphaMode::Mask ? 0.5f : (blend ? 0.01f : 0.0f);
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
    drawList(blended, 1);
    SDL_EndGPURenderPass(pass);
}

} // namespace ts2::render
