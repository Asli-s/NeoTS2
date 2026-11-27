#include "render/renderer.hpp"
#include "render/renderer_internals.hpp"

#include "diag/log.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <utility>

namespace ts2::render {

using namespace render_detail;

namespace {

// Releases the textures made for an upload that failed. Its command buffer is cancelled, so they
// were never filled and must not be drawn.
void releaseUnfilled(SDL_GPUDevice* device, const std::vector<SDL_GPUTexture*>& textures) {
    for (auto* t : textures) {
        if (t) {
            SDL_ReleaseGPUTexture(device, t);
        }
    }
}

} // namespace

void SdlGpuRenderer::releaseSprites() {
    for (auto* t : spriteTextures_) {
        if (t) {
            SDL_ReleaseGPUTexture(device_, t);
        }
    }
    spriteTextures_.clear();
    spriteSizes_.clear();
}

void SdlGpuRenderer::setSprites(const std::vector<std::pair<int, const scene::Image*>>& images) {
    if (images.empty()) {
        return;
    }
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    if (cmd == nullptr) {
        return;
    }
    UploadPass pass(device_, cmd);
    for (const auto& [index, image] : images) {
        if (index < 0 || image == nullptr || image->width == 0 || image->height == 0 ||
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
            if (!spriteTextures_[i]) {
                spriteSizes_[i] = {0, 0};
            }
        }
        // Out of GPU memory: this picture is skipped rather than the whole frame lost.
        const SDL_GPUTransferBufferLocation at =
            spriteTextures_[i] ? pass.stage(image->rgba.data(), static_cast<Uint32>(image->rgba.size())) : SDL_GPUTransferBufferLocation{};
        if (at.transfer_buffer == nullptr) {
            continue;
        }
        SDL_GPUTextureTransferInfo from{};
        from.transfer_buffer = at.transfer_buffer;
        from.offset = at.offset;
        from.pixels_per_row = image->width;
        from.rows_per_layer = image->height;
        SDL_GPUTextureRegion to{};
        to.texture = spriteTextures_[i];
        to.w = image->width;
        to.h = image->height;
        to.d = 1;
        SDL_UploadToGPUTexture(pass.copy(), &from, &to, true);
    }
    pass.end();
    SDL_SubmitGPUCommandBuffer(cmd);
}

void SdlGpuRenderer::addMaterials(const scene::Scene& scene) {
    if (scene.materials.size() <= materials_.size() && scene.images.size() <= gpu_->textures.size() &&
        scene.envCubes.size() <= gpu_->envCubes.size()) {
        return;
    }
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    if (cmd == nullptr) {
        throw sdlError("GPU command buffer");
    }
    std::vector<SDL_GPUTexture*> withMips, textures, cubes;
    try {
        UploadPass pass(device_, cmd);
        for (std::size_t i = gpu_->textures.size(); i < scene.images.size(); ++i) {
            const auto& image = scene.images[i];
            textures.push_back(complete(image) ? uploadTexture(device_, pass, image, true, withMips) : nullptr);
        }
        for (std::size_t i = gpu_->envCubes.size(); i < scene.envCubes.size(); ++i) {
            cubes.push_back(uploadCube(device_, pass, scene.envCubes[i]));
        }
        pass.end();
    } catch (...) {
        releaseUnfilled(device_, textures);
        releaseUnfilled(device_, cubes);
        throw;
    }
    gpu_->textures.insert(gpu_->textures.end(), textures.begin(), textures.end());
    gpu_->envCubes.insert(gpu_->envCubes.end(), cubes.begin(), cubes.end());
    for (auto* t : withMips) {
        SDL_GenerateMipmapsForGPUTexture(cmd, t);
    }
    SDL_SubmitGPUCommandBuffer(cmd);
    if (scene.materials.size() > materials_.size()) {
        materials_.insert(materials_.end(), scene.materials.begin() + static_cast<std::ptrdiff_t>(materials_.size()), scene.materials.end());
    }
}

void SdlGpuRenderer::addMeshes(const scene::Scene& scene) {
    if (scene.meshes.size() <= gpu_->meshes.size()) {
        return;
    }
    const auto set = static_cast<std::uint32_t>(gpu_->meshBuffers.size());
    MeshData data;
    std::vector<GpuMesh> added;
    for (std::size_t i = gpu_->meshes.size(); i < scene.meshes.size(); ++i) {
        added.push_back(packMesh(scene.meshes[i], scene.materials, set, data));
    }
    if (data.tooBig) {
        diag::warning("renderer: added meshes are too big for one GPU buffer; some are left out");
    }
    // Kept before they are filled, so that release() frees them should the upload fail halfway.
    MeshBuffers& buffers = gpu_->meshBuffers.emplace_back();
    if (!data.indices.empty()) {
        SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
        if (cmd == nullptr) {
            throw sdlError("GPU command buffer");
        }
        UploadPass pass(device_, cmd);
        buffers.vertices = filledBuffer(device_, pass, SDL_GPU_BUFFERUSAGE_VERTEX, data.vertices.data(),
                                        static_cast<Uint32>(data.vertices.size() * sizeof(float)));
        buffers.indices = filledBuffer(device_, pass, SDL_GPU_BUFFERUSAGE_INDEX, data.indices.data(),
                                       static_cast<Uint32>(data.indices.size() * sizeof(std::uint32_t)));
        if (!data.skin.empty()) {
            buffers.skin = filledBuffer(device_, pass, SDL_GPU_BUFFERUSAGE_VERTEX, data.skin.data(), static_cast<Uint32>(data.skin.size()));
        }
        if (!data.morphs.empty()) {
            buffers.morphs = filledBuffer(device_, pass, SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ, data.morphs.data(),
                                          static_cast<Uint32>(data.morphs.size() * sizeof(float)));
        }
        pass.end();
        SDL_SubmitGPUCommandBuffer(cmd);
    }
    gpu_->meshes.insert(gpu_->meshes.end(), std::make_move_iterator(added.begin()), std::make_move_iterator(added.end()));
}

void SdlGpuRenderer::updateMeshVertices(const scene::Scene& scene, std::size_t mesh,
                                        const std::vector<std::pair<std::uint32_t, std::uint32_t>>& runs) {
    if (mesh >= gpu_->meshes.size() || mesh >= scene.meshes.size() || runs.empty()) {
        return;
    }
    auto& packed = gpu_->meshes[mesh];
    const auto& source = scene.meshes[mesh];
    // A run's place in the buffer and in `floats`.
    struct Piece {
        SDL_GPUBuffer* buffer = nullptr;
        Uint32 offset = 0, size = 0, from = 0;
    };
    std::vector<Piece> pieces;
    std::vector<float> floats;
    for (auto& g : packed.primitives) {
        if (g.source >= source.primitives.size() || g.buffers >= gpu_->meshBuffers.size() || gpu_->meshBuffers[g.buffers].vertices == nullptr) {
            continue;
        }
        const auto& p = source.primitives[g.source];
        // A primitive whose vertex count changed needs the whole mesh again.
        if (p.positions.size() / 3 != g.vertices) {
            replaceMeshes(scene, {mesh});
            return;
        }
        for (const auto& [first, count] : runs) {
            if (count == 0 || first >= g.vertices || count > g.vertices - first) {
                continue;
            }
            const auto at = floats.size();
            floats.resize(at + static_cast<std::size_t>(count) * kVertexFloats);
            for (std::uint32_t i = 0; i < count; ++i) {
                packVertex(p, first + i, &floats[at + static_cast<std::size_t>(i) * kVertexFloats]);
                // The culling box grows to take in the moved points.
                for (std::size_t k = 0; k < 3; ++k) {
                    const float c = p.positions[(first + i) * 3 + k];
                    g.low[k] = std::min(g.low[k], c);
                    g.high[k] = std::max(g.high[k], c);
                }
            }
            pieces.push_back({gpu_->meshBuffers[g.buffers].vertices, static_cast<Uint32>((g.firstVertex + first) * kVertexFloats * sizeof(float)),
                              static_cast<Uint32>(static_cast<std::size_t>(count) * kVertexFloats * sizeof(float)), static_cast<Uint32>(at * sizeof(float))});
        }
    }
    if (pieces.empty()) {
        return;
    }
    SDL_GPUTransferBufferCreateInfo info{};
    info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    info.size = static_cast<Uint32>(floats.size() * sizeof(float));
    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device_, &info);
    if (transfer == nullptr) {
        replaceMeshes(scene, {mesh});
        return;
    }
    if (void* mapped = SDL_MapGPUTransferBuffer(device_, transfer, false)) {
        std::memcpy(mapped, floats.data(), floats.size() * sizeof(float));
        SDL_UnmapGPUTransferBuffer(device_, transfer);
        SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
        if (cmd != nullptr) {
            SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
            for (const auto& piece : pieces) {
                const SDL_GPUTransferBufferLocation from{transfer, piece.from};
                const SDL_GPUBufferRegion to{piece.buffer, piece.offset, piece.size};
                // Not cycled, so the rest of the buffer keeps its contents. Earlier frames read it
                // before the copy runs, as the GPU keeps the order commands were sent in.
                SDL_UploadToGPUBuffer(copy, &from, &to, false);
            }
            SDL_EndGPUCopyPass(copy);
            SDL_SubmitGPUCommandBuffer(cmd);
        }
    }
    SDL_ReleaseGPUTransferBuffer(device_, transfer);
}

void SdlGpuRenderer::replaceMeshes(const scene::Scene& scene, const std::vector<std::size_t>& indexes) {
    std::vector<std::size_t> replaced;
    for (const std::size_t i : indexes) {
        if (i < gpu_->meshes.size() && i < scene.meshes.size()) {
            replaced.push_back(i);
        }
    }
    if (replaced.empty()) {
        return;
    }
    MeshData data;
    std::vector<GpuMesh> packed;
    for (const std::size_t i : replaced) {
        packed.push_back(packMesh(scene.meshes[i], scene.materials, 0, data));
    }
    // Only unskinned meshes without morphs (walls and floors) are replaced in place.
    if (!data.skin.empty() || !data.morphs.empty()) {
        return;
    }
    const auto vertexBytes = static_cast<Uint32>(data.vertices.size() * sizeof(float));
    const auto indexBytes = static_cast<Uint32>(data.indices.size() * sizeof(std::uint32_t));
    // A buffer set only the replaced meshes draw from is filled again when the new data fits, as
    // making new buffers on every change stutters.
    std::vector<bool> isReplaced(gpu_->meshes.size(), false);
    for (const std::size_t i : replaced) {
        isReplaced[i] = true;
    }
    std::vector<std::uint32_t> theirs;
    for (const std::size_t i : replaced) {
        for (const auto& p : gpu_->meshes[i].primitives) {
            if (std::find(theirs.begin(), theirs.end(), p.buffers) == theirs.end()) {
                theirs.push_back(p.buffers);
            }
        }
    }
    std::vector<bool> usedElsewhere(gpu_->meshBuffers.size(), false);
    for (std::size_t m = 0; m < gpu_->meshes.size(); ++m) {
        for (const auto& p : gpu_->meshes[m].primitives) {
            if (!isReplaced[m] && p.buffers < usedElsewhere.size()) {
                usedElsewhere[p.buffers] = true;
            }
        }
    }
    const auto fits = [&](std::uint32_t s) {
        if (s >= gpu_->meshBuffers.size() || usedElsewhere[s] || !data.skin.empty() || !data.morphs.empty()) {
            return false;
        }
        const auto& b = gpu_->meshBuffers[s];
        return b.vertices != nullptr && b.indices != nullptr && b.skin == nullptr && b.morphs == nullptr && vertexBytes <= b.vertexCapacity &&
               indexBytes <= b.indexCapacity;
    };
    const auto own = std::find_if(theirs.begin(), theirs.end(), fits);
    const bool refill = own != theirs.end();
    std::uint32_t set = 0;
    if (refill) {
        set = *own;
    } else {
        // Otherwise a freed set is reused when there is one.
        set = static_cast<std::uint32_t>(gpu_->meshBuffers.size());
        for (std::size_t s = 0; s < gpu_->meshBuffers.size(); ++s) {
            const auto& b = gpu_->meshBuffers[s];
            if (!usedElsewhere[s] && std::find(theirs.begin(), theirs.end(), static_cast<std::uint32_t>(s)) == theirs.end() && b.vertices == nullptr &&
                b.indices == nullptr && b.skin == nullptr && b.morphs == nullptr) {
                set = static_cast<std::uint32_t>(s);
                break;
            }
        }
        if (set == gpu_->meshBuffers.size()) {
            gpu_->meshBuffers.emplace_back();
        }
    }
    for (auto& mesh : packed) {
        for (auto& p : mesh.primitives) {
            p.buffers = set;
        }
    }
    if (!data.indices.empty()) {
        SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
        if (cmd == nullptr) {
            throw sdlError("GPU command buffer");
        }
        UploadPass pass(device_, cmd);
        MeshBuffers& buffers = gpu_->meshBuffers[set];
        // New buffers get room to spare, so a slightly larger change still fits.
        const auto fill = [&](SDL_GPUBuffer*& buffer, std::uint32_t& capacity, SDL_GPUBufferUsageFlags usage, const void* bytes, Uint32 size) {
            if (!refill) {
                SDL_GPUBufferCreateInfo info{};
                info.usage = usage;
                info.size = size + size / 2;
                buffer = SDL_CreateGPUBuffer(device_, &info);
                if (buffer == nullptr) {
                    throw sdlError("GPU buffer");
                }
                capacity = info.size;
            }
            const SDL_GPUTransferBufferLocation from = pass.stage(bytes, size);
            if (from.transfer_buffer == nullptr) {
                throw sdlError("GPU upload buffer");
            }
            const SDL_GPUBufferRegion to{buffer, 0, size};
            // Cycled, so frames still drawing keep the old contents.
            SDL_UploadToGPUBuffer(pass.copy(), &from, &to, refill);
        };
        fill(buffers.vertices, buffers.vertexCapacity, SDL_GPU_BUFFERUSAGE_VERTEX, data.vertices.data(), vertexBytes);
        fill(buffers.indices, buffers.indexCapacity, SDL_GPU_BUFFERUSAGE_INDEX, data.indices.data(), indexBytes);
        pass.end();
        SDL_SubmitGPUCommandBuffer(cmd);
    }
    for (std::size_t k = 0; k < replaced.size(); ++k) {
        gpu_->meshes[replaced[k]] = std::move(packed[k]);
    }
    // Sets no mesh draws from are released. SDL frees them once the frames using them are done.
    std::vector<bool> used = std::move(usedElsewhere);
    used.resize(gpu_->meshBuffers.size(), false);
    used[set] = true;
    for (std::size_t s = 0; s < gpu_->meshBuffers.size(); ++s) {
        if (used[s]) {
            continue;
        }
        for (auto** b : {&gpu_->meshBuffers[s].vertices, &gpu_->meshBuffers[s].indices, &gpu_->meshBuffers[s].skin, &gpu_->meshBuffers[s].morphs}) {
            if (*b) {
                SDL_ReleaseGPUBuffer(device_, *b);
                *b = nullptr;
            }
        }
    }
}

void SdlGpuRenderer::GpuAddition::release() {
    for (auto* list : {&textures, &cubes}) {
        for (auto* t : *list) {
            if (t) {
                SDL_ReleaseGPUTexture(device, t);
            }
        }
        list->clear();
    }
    for (auto** b : {&buffers.vertices, &buffers.indices, &buffers.skin, &buffers.morphs}) {
        if (*b) {
            SDL_ReleaseGPUBuffer(device, *b);
            *b = nullptr;
        }
    }
    withMips.clear();
    meshes.clear();
}

IRenderer::Addition SdlGpuRenderer::prepareAddition(const scene::Scene& add, const std::vector<int>& shared) const {
    auto a = std::make_unique<GpuAddition>();
    a->device = device_;
    a->materials = add.materials.size();
    MeshData data;
    for (const auto& mesh : add.meshes) {
        a->meshes.push_back(packMesh(mesh, add.materials, 0, data));
    }
    if (data.tooBig) {
        diag::warning("renderer: added meshes are too big for one GPU buffer; some are left out");
    }
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    if (cmd == nullptr) {
        throw sdlError("GPU command buffer");
    }
    UploadPass pass(device_, cmd);
    for (std::size_t i = 0; i < add.images.size(); ++i) {
        if (i < shared.size() && shared[i] >= 0) {
            continue;
        }
        const auto& image = add.images[i];
        a->textures.push_back(complete(image) ? uploadTexture(device_, pass, image, true, a->withMips) : nullptr);
    }
    for (const auto& cube : add.envCubes) {
        a->cubes.push_back(uploadCube(device_, pass, cube));
    }
    if (!data.indices.empty()) {
        a->buffers.vertices = filledBuffer(device_, pass, SDL_GPU_BUFFERUSAGE_VERTEX, data.vertices.data(),
                                           static_cast<Uint32>(data.vertices.size() * sizeof(float)));
        a->buffers.indices = filledBuffer(device_, pass, SDL_GPU_BUFFERUSAGE_INDEX, data.indices.data(),
                                          static_cast<Uint32>(data.indices.size() * sizeof(std::uint32_t)));
        if (!data.skin.empty()) {
            a->buffers.skin = filledBuffer(device_, pass, SDL_GPU_BUFFERUSAGE_VERTEX, data.skin.data(), static_cast<Uint32>(data.skin.size()));
        }
        if (!data.morphs.empty()) {
            a->buffers.morphs = filledBuffer(device_, pass, SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ, data.morphs.data(),
                                             static_cast<Uint32>(data.morphs.size() * sizeof(float)));
        }
    }
    pass.end();
    SDL_SubmitGPUCommandBuffer(cmd);
    return a;
}

void SdlGpuRenderer::leaveOutPrepared(Addition& addition, const std::vector<std::size_t>& positions) const {
    auto* a = dynamic_cast<GpuAddition*>(addition.get());
    if (a == nullptr) {
        return;
    }
    std::vector<std::size_t> sorted = positions;
    std::sort(sorted.begin(), sorted.end());
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
    // From the last, so the positions before stay where they are.
    for (auto it = sorted.rbegin(); it != sorted.rend(); ++it) {
        if (*it >= a->textures.size()) {
            continue;
        }
        const auto at = a->textures.begin() + static_cast<std::ptrdiff_t>(*it);
        if (SDL_GPUTexture* t = *at) {
            std::erase(a->withMips, t);
            SDL_ReleaseGPUTexture(device_, t);
        }
        a->textures.erase(at);
    }
}

void SdlGpuRenderer::addPrepared(const scene::Scene& scene, Addition addition) {
    auto* a = dynamic_cast<GpuAddition*>(addition.get());
    // It fits only when what it brings is exactly what the scene has gained past what is drawn.
    const bool fits = a != nullptr && gpu_ && a->device == device_ && gpu_->textures.size() + a->textures.size() == scene.images.size() &&
                      gpu_->envCubes.size() + a->cubes.size() == scene.envCubes.size() &&
                      gpu_->meshes.size() + a->meshes.size() == scene.meshes.size() && scene.materials.size() >= a->materials &&
                      materials_.size() <= scene.materials.size() - a->materials;
    if (!fits) {
        addition.reset();
        addMaterials(scene);
        addMeshes(scene);
        return;
    }
    gpu_->textures.insert(gpu_->textures.end(), a->textures.begin(), a->textures.end());
    gpu_->envCubes.insert(gpu_->envCubes.end(), a->cubes.begin(), a->cubes.end());
    a->textures.clear();
    a->cubes.clear();
    const auto firstMaterial = static_cast<int>(scene.materials.size() - a->materials);
    materials_.insert(materials_.end(), scene.materials.begin() + static_cast<std::ptrdiff_t>(materials_.size()), scene.materials.end());
    const auto set = static_cast<std::uint32_t>(gpu_->meshBuffers.size());
    gpu_->meshBuffers.push_back(a->buffers);
    a->buffers = {};
    for (auto& mesh : a->meshes) {
        for (auto& p : mesh.primitives) {
            p.buffers = set;
            if (p.material >= 0) {
                p.material += firstMaterial;
            }
        }
    }
    gpu_->meshes.insert(gpu_->meshes.end(), std::make_move_iterator(a->meshes.begin()), std::make_move_iterator(a->meshes.end()));
    a->meshes.clear();
    // SDL makes mip levels with pipelines it keeps without a lock: only here, on the drawing thread.
    if (!a->withMips.empty()) {
        if (SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_)) {
            for (auto* t : a->withMips) {
                SDL_GenerateMipmapsForGPUTexture(cmd, t);
            }
            SDL_SubmitGPUCommandBuffer(cmd);
        }
        a->withMips.clear();
    }
}

void SdlGpuRenderer::setImages(const scene::Scene& scene, const std::vector<int>& indexes, bool fade) {
    if (indexes.empty()) {
        return;
    }
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    if (cmd == nullptr) {
        throw sdlError("GPU command buffer");
    }
    std::vector<SDL_GPUTexture*> withMips, made;
    std::vector<int> madeFor;
    try {
        UploadPass pass(device_, cmd);
        for (const int i : indexes) {
            const auto at = static_cast<std::size_t>(i);
            if (i < 0 || at >= scene.images.size() || at >= gpu_->textures.size() || !complete(scene.images[at])) {
                continue;
            }
            made.push_back(uploadTexture(device_, pass, scene.images[at], false, withMips));
            madeFor.push_back(i);
        }
        pass.end();
    } catch (...) {
        releaseUnfilled(device_, made);
        throw;
    }
    for (std::size_t n = 0; n < made.size(); ++n) {
        const int i = madeFor[n];
        // SDL frees a released texture once the frames drawing with it are done.
        SDL_GPUTexture* old = std::exchange(gpu_->textures[static_cast<std::size_t>(i)], made[n]);
        auto& from = gpu_->fadingFrom[i];
        if (fade && from == nullptr) {
            from = old;  // the light seen when the fade began
        } else if (old != nullptr) {
            SDL_ReleaseGPUTexture(device_, old);
        }
        if (!fade && from != nullptr) {
            SDL_ReleaseGPUTexture(device_, from);
        }
        if (!fade) {
            gpu_->fadingFrom.erase(i);
        }
    }
    SDL_SubmitGPUCommandBuffer(cmd);
}

void SdlGpuRenderer::setImagePixels(int index, const scene::Image& image) {
    const auto at = static_cast<std::size_t>(index);
    if (index < 0 || at >= gpu_->textures.size() || !complete(image)) {
        return;
    }
    const auto bytes = static_cast<Uint32>(image.rgba.size());
    auto& s = gpu_->streamed[index];
    if (s.width != image.width || s.height != image.height || s.transfer == nullptr) {
        // A texture of its own, one level, which later frames write into.
        SDL_GPUTextureCreateInfo info{};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        info.width = image.width;
        info.height = image.height;
        info.layer_count_or_depth = 1;
        info.num_levels = 1;
        SDL_GPUTexture* fresh = SDL_CreateGPUTexture(device_, &info);
        SDL_GPUTransferBufferCreateInfo buffer{};
        buffer.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        buffer.size = bytes;
        SDL_GPUTransferBuffer* transfer = fresh ? SDL_CreateGPUTransferBuffer(device_, &buffer) : nullptr;
        if (transfer == nullptr) {
            if (fresh) {
                SDL_ReleaseGPUTexture(device_, fresh);
            }
            gpu_->streamed.erase(index);
            return;
        }
        if (s.transfer) {
            SDL_ReleaseGPUTransferBuffer(device_, s.transfer);
        }
        if (SDL_GPUTexture* old = std::exchange(gpu_->textures[at], fresh)) {
            SDL_ReleaseGPUTexture(device_, old);  // freed once the frames drawing with it are done
        }
        s = {image.width, image.height, transfer};
    }
    // Cycling lets the GPU keep drawing earlier frames from the old contents without a wait.
    void* mapped = SDL_MapGPUTransferBuffer(device_, s.transfer, true);
    if (mapped == nullptr) {
        return;
    }
    std::memcpy(mapped, image.rgba.data(), bytes);
    SDL_UnmapGPUTransferBuffer(device_, s.transfer);
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    if (cmd == nullptr) {
        throw sdlError("GPU command buffer");
    }
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureTransferInfo from{};
    from.transfer_buffer = s.transfer;
    from.pixels_per_row = image.width;
    from.rows_per_layer = image.height;
    SDL_GPUTextureRegion to{};
    to.texture = gpu_->textures[at];
    to.w = image.width;
    to.h = image.height;
    to.d = 1;
    SDL_UploadToGPUTexture(copy, &from, &to, true);
    SDL_EndGPUCopyPass(copy);
    SDL_SubmitGPUCommandBuffer(cmd);
}

void SdlGpuRenderer::setLightMapFadeFrom(const std::map<int, scene::Image>& images) {
    if (images.empty()) {
        return;
    }
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    if (cmd == nullptr) {
        throw sdlError("GPU command buffer");
    }
    std::vector<SDL_GPUTexture*> withMips, made;
    std::vector<int> madeFor;
    try {
        UploadPass pass(device_, cmd);
        for (const auto& [index, image] : images) {
            if (index < 0 || static_cast<std::size_t>(index) >= gpu_->textures.size() || !complete(image)) {
                continue;
            }
            made.push_back(uploadTexture(device_, pass, image, false, withMips));
            madeFor.push_back(index);
        }
        pass.end();
    } catch (...) {
        releaseUnfilled(device_, made);
        throw;
    }
    for (std::size_t n = 0; n < made.size(); ++n) {
        if (SDL_GPUTexture* old = std::exchange(gpu_->fadingFrom[madeFor[n]], made[n])) {
            SDL_ReleaseGPUTexture(device_, old);
        }
    }
    SDL_SubmitGPUCommandBuffer(cmd);
}

void SdlGpuRenderer::setLightMapFade(float fade) {
    gpu_->lightMapFade = std::clamp(fade, 0.0f, 1.0f);
    if (gpu_->lightMapFade >= 1) {
        for (const auto& [index, texture] : gpu_->fadingFrom) {
            if (texture != nullptr) {
                SDL_ReleaseGPUTexture(device_, texture);
            }
        }
        gpu_->fadingFrom.clear();
    }
}

void SdlGpuRenderer::setSkybox(const std::vector<scene::Image>& faces) {
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    if (cmd == nullptr) {
        throw sdlError("GPU command buffer");
    }
    SDL_GPUTexture* fresh = nullptr;
    {
        UploadPass pass(device_, cmd);
        fresh = uploadCube(device_, pass, faces);
        pass.end();
    }
    SDL_SubmitGPUCommandBuffer(cmd);
    if (gpu_->skybox != nullptr) {
        SDL_ReleaseGPUTexture(device_, gpu_->skybox);
    }
    gpu_->skybox = fresh;
}

void SdlGpuRenderer::uploadJoints(SDL_GPUCommandBuffer* cmd) {
    if (!jointsChanged_ || jointData_.empty()) {
        return;
    }
    if (jointData_.size() > jointCapacity_) {
        if (jointBuffer_) {
            SDL_ReleaseGPUBuffer(device_, jointBuffer_);
        }
        jointCapacity_ = std::max(jointData_.size(), jointCapacity_ * 2);
        SDL_GPUBufferCreateInfo info{};
        info.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
        info.size = static_cast<Uint32>(jointCapacity_ * sizeof(float));
        jointBuffer_ = SDL_CreateGPUBuffer(device_, &info);
        if (jointBuffer_ == nullptr) {
            jointCapacity_ = 0;
        }
    }
    // Out of GPU memory: the poses are sent again next frame.
    const auto bytes = static_cast<Uint32>(jointData_.size() * sizeof(float));
    void* mapped = jointBuffer_ ? mapStream(device_, jointUpload_, bytes) : nullptr;
    if (mapped == nullptr) {
        return;
    }
    std::memcpy(mapped, jointData_.data(), bytes);
    SDL_UnmapGPUTransferBuffer(device_, jointUpload_.buffer);
    jointsChanged_ = false;
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTransferBufferLocation from{jointUpload_.buffer, 0};
    SDL_GPUBufferRegion to{jointBuffer_, 0, bytes};
    SDL_UploadToGPUBuffer(copy, &from, &to, true);
    SDL_EndGPUCopyPass(copy);
}

void SdlGpuRenderer::uploadSpriteVertices(SDL_GPUCommandBuffer* cmd, unsigned, unsigned) {
    if (sprites_.empty()) {
        return;
    }
    const auto count = static_cast<unsigned>(sprites_.size() * 6);
    if (count > spriteCapacity_) {
        if (spriteVertices_) {
            SDL_ReleaseGPUBuffer(device_, spriteVertices_);
        }
        spriteCapacity_ = std::max(count, spriteCapacity_ * 2);
        SDL_GPUBufferCreateInfo info{};
        info.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
        info.size = spriteCapacity_ * kSpriteFloats * static_cast<Uint32>(sizeof(float));
        spriteVertices_ = SDL_CreateGPUBuffer(device_, &info);
        if (spriteVertices_ == nullptr) {
            spriteCapacity_ = 0;
        }
    }
    const auto bytes = count * kSpriteFloats * static_cast<Uint32>(sizeof(float));
    void* mapped = spriteVertices_ ? mapStream(device_, spriteUpload_, bytes) : nullptr;
    if (mapped == nullptr) {
        return;
    }
    auto* out = static_cast<float*>(mapped);
    for (const auto& s : sprites_) {
        const float corners[6][4] = {{s.x0, s.y0, s.u0, s.v0}, {s.x1, s.y0, s.u1, s.v0}, {s.x1, s.y1, s.u1, s.v1},
                                     {s.x0, s.y0, s.u0, s.v0}, {s.x1, s.y1, s.u1, s.v1}, {s.x0, s.y1, s.u0, s.v1}};
        for (const auto& c : corners) {
            const float vertex[kSpriteFloats] = {c[0], c[1], c[2], c[3], s.color[0], s.color[1], s.color[2], s.color[3]};
            std::memcpy(out, vertex, sizeof(vertex));
            out += kSpriteFloats;
        }
    }
    SDL_UnmapGPUTransferBuffer(device_, spriteUpload_.buffer);
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTransferBufferLocation from{spriteUpload_.buffer, 0};
    SDL_GPUBufferRegion to{spriteVertices_, 0, bytes};
    SDL_UploadToGPUBuffer(copy, &from, &to, false);
    SDL_EndGPUCopyPass(copy);
}

void SdlGpuRenderer::uploadParticleVertices(SDL_GPUCommandBuffer* cmd) {
    if (particleVertices_.empty()) {
        return;
    }
    const auto count = static_cast<std::uint32_t>(particleVertices_.size());
    if (count > particleCapacity_) {
        if (particleBuffer_) {
            SDL_ReleaseGPUBuffer(device_, particleBuffer_);
        }
        particleCapacity_ = std::max(count, particleCapacity_ * 2);
        SDL_GPUBufferCreateInfo info{};
        info.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
        info.size = particleCapacity_ * static_cast<Uint32>(sizeof(scene::ParticleVertex));
        particleBuffer_ = SDL_CreateGPUBuffer(device_, &info);
        if (particleBuffer_ == nullptr) {
            particleCapacity_ = 0;
        }
    }
    const auto bytes = count * static_cast<Uint32>(sizeof(scene::ParticleVertex));
    void* mapped = particleBuffer_ ? mapStream(device_, particleUpload_, bytes) : nullptr;
    if (mapped == nullptr) {
        return;
    }
    std::memcpy(mapped, particleVertices_.data(), bytes);
    SDL_UnmapGPUTransferBuffer(device_, particleUpload_.buffer);
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTransferBufferLocation from{particleUpload_.buffer, 0};
    SDL_GPUBufferRegion to{particleBuffer_, 0, bytes};
    SDL_UploadToGPUBuffer(copy, &from, &to, false);
    SDL_EndGPUCopyPass(copy);
}

} // namespace ts2::render
