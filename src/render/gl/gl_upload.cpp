#include "diag/log.hpp"
#include "diag/timing.hpp"
#include "render/gl/gl_renderer.hpp"
#include "render/image_levels.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <thread>
#include <utility>

namespace ts2::render {

void GlRenderer::SceneGpu::release(Graveyard& graveyard) {
    std::lock_guard lock(graveyard.lock);
    const auto give = [&](Texture& t) {
        if (t.id != 0) {
            graveyard.textures.push_back(t.id);
            t.id = 0;
        }
    };
    for (auto& t : textures) give(t);
    textures.clear();
    for (auto& [index, t] : fadingFrom) give(t);
    fadingFrom.clear();
    give(white);
    give(blackCube);
    give(skybox);
    for (auto& t : envCubes) give(t);
    envCubes.clear();
    for (auto& b : meshBuffers) {
        for (GLuint* id : {&b.vertices, &b.indices}) {
            if (*id != 0) {
                graveyard.buffers.push_back(*id);
                *id = 0;
            }
        }
    }
    meshBuffers.clear();
    meshes.clear();
    for (auto* list : {&terrainPaints, &roadTextures, &imposterTextures}) {
        for (auto& t : *list) give(t);
        list->clear();
    }
    for (auto* pair : {&terrainAlphas, &terrainCells}) {
        for (auto& t : *pair) give(t);
    }
    give(terrainLight);
    give(terrainGradient);
    give(ripples);
    give(skyCube);
    for (GLuint* id : {&waterVertices, &waterIndices, &terrainVertices, &terrainIndices, &roadVertices, &roadIndices, &imposterVertices, &imposterIndices, &curtainVertices, &curtainIndices}) {
        if (*id != 0) {
            graveyard.buffers.push_back(*id);
            *id = 0;
        }
    }
    terrainIndexCount = 0;
    waterIndexCount = 0;
    roadRuns.clear();
    imposterDraws.clear();
}

bool GlRenderer::SceneGpu::releaseTextures(Graveyard& graveyard, std::size_t count) {
    std::lock_guard lock(graveyard.lock);
    for (auto* list : {&textures, &imposterTextures}) {
        for (; count > 0 && !list->empty(); --count) {
            if (list->back().id != 0) {
                graveyard.textures.push_back(list->back().id);
            }
            list->pop_back();
        }
    }
    return textures.empty() && imposterTextures.empty();
}

void GlRenderer::GpuPrepared::releaseSome(std::size_t count) {
    if (gpu && graveyard && gpu->releaseTextures(*graveyard, count)) {
        release();
    }
}

void GlRenderer::GpuPrepared::release() {
    if (gpu && graveyard) {
        gpu->release(*graveyard);
    }
    gpu.reset();
    staged.reset();
}

// Deletes what prepared scenes gave up, on the drawing thread.
void GlRenderer::collectGarbage() {
    std::vector<GLuint> textures, buffers;
    {
        std::lock_guard lock(graveyard_->lock);
        textures.swap(graveyard_->textures);
        buffers.swap(graveyard_->buffers);
    }
    if (!textures.empty()) {
        gl_.DeleteTextures(static_cast<GLsizei>(textures.size()), textures.data());
    }
    if (!buffers.empty()) {
        gl_.DeleteBuffers(static_cast<GLsizei>(buffers.size()), buffers.data());
    }
}

void GlRenderer::release(Texture& texture) {
    if (texture.id != 0) {
        gl_.DeleteTextures(1, &texture.id);
        texture.id = 0;
    }
}

void GlRenderer::releaseBuffer(GLuint& buffer) {
    if (buffer != 0) {
        gl_.DeleteBuffers(1, &buffer);
        buffer = 0;
    }
}

// A texture of a complete image, with mip levels unless `mips` is false.
GlRenderer::Texture GlRenderer::uploadTexture(const scene::Image& image, bool mips, bool anisotropic) {
    // One the driver can't hold is left out, and is drawn as the white one is.
    const auto largest = static_cast<std::uint32_t>(gl_.maxTextureSize);
    if (image.width > largest || image.height > largest) {
        return Texture{};
    }
    Texture t;
    t.target = GL_TEXTURE_2D;
    t.mips = mips && (image.width > 1 || image.height > 1);
    gl_.GenTextures(1, &t.id);
    gl_.ActiveTexture(GL_TEXTURE0);
    gl_.BindTexture(GL_TEXTURE_2D, t.id);
    forgetBindings();
    gl_.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    gl_.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, t.mips ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    gl_.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl_.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    gl_.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    t.wrap = GL_REPEAT;
    if (t.mips && anisotropic && gl_.anisotropy) {
        gl_.TexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, std::min(8.0f, gl_.maxAnisotropy));
    }
    const bool own = t.mips && ownLevels(image);
    // The driver makes the smaller levels as the first is uploaded, unless the image has its own.
    if (t.mips && !own) {
        gl_.TexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, GL_TRUE);
    }
    gl_.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, static_cast<GLsizei>(image.width), static_cast<GLsizei>(image.height), 0, GL_RGBA,
                   GL_UNSIGNED_BYTE, image.rgba.data());
    if (own) {
        std::uint32_t w = image.width, h = image.height;
        for (std::size_t level = 0; level < image.smallerLevels.size(); ++level) {
            w = std::max(1u, w / 2);
            h = std::max(1u, h / 2);
            gl_.TexImage2D(GL_TEXTURE_2D, static_cast<GLint>(level + 1), GL_RGBA8, static_cast<GLsizei>(w), static_cast<GLsizei>(h), 0,
                           GL_RGBA, GL_UNSIGNED_BYTE, image.smallerLevels[level].data());
        }
    }
    return t;
}

// A cube map of six faces of one size. Empty when the faces are not that.
GlRenderer::Texture GlRenderer::uploadCube(const std::vector<scene::Image>& faces, bool mips) {
    Texture t;
    if (faces.size() != 6 || faces.front().width == 0 || faces.front().width != faces.front().height) {
        return t;
    }
    const auto& face = faces.front();
    if (face.width > static_cast<std::uint32_t>(gl_.maxTextureSize)) {
        return t;
    }
    for (const auto& f : faces) {
        if (f.width != face.width || f.height != face.height || !complete(f)) {
            return t;
        }
    }
    t.target = GL_TEXTURE_CUBE_MAP;
    t.wrap = GL_CLAMP_TO_EDGE;
    gl_.GenTextures(1, &t.id);
    gl_.ActiveTexture(GL_TEXTURE0);
    gl_.BindTexture(GL_TEXTURE_CUBE_MAP, t.id);
    forgetBindings();
    gl_.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    t.mips = mips && face.width > 1;
    gl_.TexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, t.mips ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    gl_.TexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    if (t.mips) {
        gl_.TexParameteri(GL_TEXTURE_CUBE_MAP, GL_GENERATE_MIPMAP, GL_TRUE);
    }
    gl_.TexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl_.TexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    for (GLenum l = 0; l < 6; ++l) {
        gl_.TexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + l, 0, GL_RGBA8, static_cast<GLsizei>(face.width), static_cast<GLsizei>(face.height), 0,
                       GL_RGBA, GL_UNSIGNED_BYTE, faces[l].rgba.data());
    }
    return t;
}

GlRenderer::MeshBuffers GlRenderer::uploadMeshBuffers(MeshData& data) {
    MeshBuffers b;
    if (data.indices.empty()) {
        return b;
    }
    b.vertices = uploadBuffer(GL_ARRAY_BUFFER, data.vertices.data(), data.vertices.size() * sizeof(float));
    b.indices = uploadBuffer(GL_ELEMENT_ARRAY_BUFFER, data.indices.data(), data.indices.size() * sizeof(std::uint32_t));
    // Skinned primitives are posed on the CPU from the vertices as they were made.
    if (!data.skin.empty()) {
        b.cpuVertices = std::move(data.vertices);
        b.cpuSkin = std::move(data.skin);
    }
    return b;
}

// What a scene is uploaded from, gathered on any thread.
// The images are copied unless `copyImages` is false, when the scene is to be alive for the upload.
std::unique_ptr<GlRenderer::Staged> GlRenderer::stage(const scene::Scene& scene, bool copyImages) {
    auto s = std::make_unique<Staged>();
    if (copyImages) {
        s->images = scene.images;
    } else {
        s->borrowed = &scene.images;
    }
    if (scene.lightMaps) {
        for (const int i : scene.lightMaps->floors) s->noMips.insert(static_cast<std::size_t>(i));
        for (const int i : scene.lightMaps->floorIncidence) s->noMips.insert(static_cast<std::size_t>(i));
        s->noMips.insert(static_cast<std::size_t>(scene.lightMaps->walls));
        s->noMips.insert(static_cast<std::size_t>(scene.lightMaps->wallIncidence));
    }
    for (const auto& mesh : scene.meshes) {
        s->meshes.push_back(packMesh(mesh, scene.materials.size(), 0, s->meshData));
    }
    if (s->meshData.tooBig) {
        diag::warning("renderer: the scene's meshes are too big for one GPU buffer; some are left out");
    }
    s->skybox = scene.skybox;
    s->envCubes = scene.envCubes;
    if (scene.terrain && scene.terrain->paints() > 0 && !scene.terrain->textures.empty()) {
        s->terrain = scene.terrain;
    }
    s->clip = scene.clip;
    return s;
}

void GlRenderer::uploadStaged(SceneGpu& gpu, Staged& staged) {
    diag::Timer timer("upload scene");
    const std::vector<scene::Image>& images = staged.borrowed ? *staged.borrowed : staged.images;
    gpu.textures.reserve(images.size());
    for (std::size_t i = 0; i < images.size(); ++i) {
        gpu.textures.push_back(complete(images[i]) ? uploadTexture(images[i], !staged.noMips.contains(i)) : Texture{});
        if (!staged.borrowed) {
            // The pixels are on the GPU: the copy made to carry them here isn't needed any more.
            staged.images[i] = scene::Image{};
        }
    }
    gpu.white = uploadTexture({1, 1, {255, 255, 255, 255}, {}}, false);
    gpu.meshes = std::move(staged.meshes);
    gpu.meshBuffers.push_back(uploadMeshBuffers(staged.meshData));
    gpu.skybox = uploadCube(staged.skybox);
    for (const auto& faces : staged.envCubes) {
        gpu.envCubes.push_back(uploadCube(faces));
    }
    gpu.blackCube = uploadCube(std::vector<scene::Image>(6, scene::Image{1, 1, {0, 0, 0, 255}, {}}));
    if (staged.terrain) {
        uploadTerrain(gpu, *staged.terrain);
    }
    if (staged.clip) {
        const IndexedMesh curtain = boundaryCurtain(*staged.clip);
        gpu.curtainVertices = uploadBuffer(GL_ARRAY_BUFFER, curtain.vertices.data(), curtain.vertices.size() * sizeof(float));
        gpu.curtainIndices = uploadBuffer(GL_ELEMENT_ARRAY_BUFFER, curtain.indices.data(), curtain.indices.size() * sizeof(std::uint32_t));
    }
}

void GlRenderer::forgetBindings() {
    if (onDrawingThread()) {
        bound_ = {};
        arrayBuffer_ = elementBuffer_ = ~0u;
    }
}

// Makes contexts that share their textures and buffers with the drawing one, for prepare() to use
// on other threads. Made with the drawing context current, which they then share with.
void GlRenderer::makeSharedContexts(unsigned count) {
    for (unsigned i = 0; i < count; ++i) {
        platform::GlContextPtr shared = window_->createGlContext(true);
        if (shared == nullptr) {
            break;
        }
        pool_.push_back(SharedContext{std::move(shared), false});
    }
    // Making one current again is what made them current here.
    window_->makeCurrent(context_.get());
    // Some systems (EGL on Wayland, macOS) can't make a context current on the window from another thread.
    // One is tried on a thread now, and the pool is given up when that fails.
    if (!pool_.empty()) {
        bool works = false;
        std::thread([&] {
            works = window_->makeCurrent(pool_.front().context.get());
            if (works) {
                window_->makeCurrent(nullptr);
            }
        }).join();
        if (!works) {
            destroySharedContexts();
            closing_ = false;
            window_->makeCurrent(context_.get());
        }
    }
}

void GlRenderer::destroySharedContexts() {
    std::unique_lock lock(poolLock_);
    // Waits for the contexts a thread is uploading in, and lets none be taken after this.
    closing_ = true;
    poolFree_.notify_all();
    poolFree_.wait(lock, [&] {
        for (const auto& shared : pool_) {
            if (shared.busy) {
                return false;
            }
        }
        return true;
    });
    pool_.clear();
}

// Uploads a scene on this thread in a context of its own. The objects it makes are the drawing context's
// too, once the driver is done with them (waited for here).
std::unique_ptr<GlRenderer::SceneGpu> GlRenderer::uploadShared(const scene::Scene& scene) const {
    SharedContext* mine = nullptr;
    {
        std::unique_lock lock(poolLock_);
        poolFree_.wait(lock, [&] {
            if (closing_) {
                return true;
            }
            for (auto& shared : pool_) {
                if (!shared.busy) {
                    mine = &shared;
                    return true;
                }
            }
            return false;
        });
        if (mine == nullptr) {
            return nullptr;
        }
        mine->busy = true;
    }
    std::unique_ptr<SceneGpu> gpu;
    if (window_->makeCurrent(mine->context.get())) {
        // This touches nothing but what this thread made: the caches of what is bound are the drawing thread's.
        auto* self = const_cast<GlRenderer*>(this);
        auto staged = stage(scene, false);  // the scene is alive until this returns
        gpu = std::make_unique<SceneGpu>();
        try {
            self->uploadStaged(*gpu, *staged);
            self->gl_.Finish();
        } catch (...) {
            gpu->release(*graveyard_);
            gpu.reset();
        }
        window_->makeCurrent(nullptr);
    }
    {
        std::lock_guard lock(poolLock_);
        mine->busy = false;
    }
    poolFree_.notify_all();
    return gpu;
}

IRenderer::Prepared GlRenderer::prepare(const scene::Scene& scene) const {
    auto p = std::make_unique<GpuPrepared>();
    p->graveyard = graveyard_;
    // On the drawing thread there is no need: swap uploads what is staged there and then.
    if (!pool_.empty() && !onDrawingThread()) {
        p->gpu = uploadShared(scene);
    }
    if (!p->gpu) {
        p->staged = stage(scene);
    }
    copyScene(*p, scene);
    return Prepared(std::move(p));
}

void GlRenderer::upload(Prepared prepared) {
    (void)swap(std::move(prepared));
}

IRenderer::Prepared GlRenderer::swap(Prepared prepared) {
    if (!prepared.get()) {
        prepared = Prepared(std::make_unique<GpuPrepared>());
    }
    auto* made = dynamic_cast<GpuPrepared*>(prepared.get());
    if (made == nullptr) {
        throw std::logic_error("a scene prepared by another renderer was given to the OpenGL 2 one");
    }
    auto& p = *made;
    p.graveyard = graveyard_;
    if (!p.gpu) {
        p.gpu = std::make_unique<SceneGpu>();
        if (p.staged) {
            uploadStaged(*p.gpu, *p.staged);
            p.staged.reset();
        }
    }
    std::swap(gpu_, p.gpu);
    swapScene(p);
    collectGarbage();
    return prepared;
}

void GlRenderer::addMaterials(const scene::Scene& scene) {
    for (std::size_t i = gpu_->textures.size(); i < scene.images.size(); ++i) {
        const auto& image = scene.images[i];
        gpu_->textures.push_back(complete(image) ? uploadTexture(image, true) : Texture{});
    }
    for (std::size_t i = gpu_->envCubes.size(); i < scene.envCubes.size(); ++i) {
        gpu_->envCubes.push_back(uploadCube(scene.envCubes[i]));
    }
    if (scene.materials.size() > materials_.size()) {
        materials_.insert(materials_.end(), scene.materials.begin() + static_cast<std::ptrdiff_t>(materials_.size()), scene.materials.end());
    }
}

void GlRenderer::addMeshes(const scene::Scene& scene) {
    if (scene.meshes.size() <= gpu_->meshes.size()) {
        return;
    }
    const auto set = static_cast<std::uint32_t>(gpu_->meshBuffers.size());
    MeshData data;
    std::vector<GpuMesh> added;
    for (std::size_t i = gpu_->meshes.size(); i < scene.meshes.size(); ++i) {
        added.push_back(packMesh(scene.meshes[i], scene.materials.size(), set, data));
    }
    if (data.tooBig) {
        diag::warning("renderer: added meshes are too big for one GPU buffer; some are left out");
    }
    gpu_->meshBuffers.push_back(uploadMeshBuffers(data));
    gpu_->meshes.insert(gpu_->meshes.end(), std::make_move_iterator(added.begin()), std::make_move_iterator(added.end()));
}

void GlRenderer::setImages(const scene::Scene& scene, const std::vector<int>& indexes, bool fade) {
    for (const int i : indexes) {
        const auto at = static_cast<std::size_t>(i);
        if (i < 0 || at >= scene.images.size() || at >= gpu_->textures.size() || !complete(scene.images[at])) {
            continue;
        }
        // Light maps are drawn as the game draws them: one level, no mipmaps.
        Texture fresh = uploadTexture(scene.images[at], false);
        Texture old = std::exchange(gpu_->textures[at], fresh);
        auto& from = gpu_->fadingFrom[i];
        if (fade && from.id == 0) {
            from = old;  // the light seen when the fade began
        } else {
            release(old);
        }
        if (!fade) {
            release(from);
            gpu_->fadingFrom.erase(i);
        }
    }
}

void GlRenderer::setLightMapFade(float fade) {
    gpu_->lightMapFade = std::clamp(fade, 0.0f, 1.0f);
    if (gpu_->lightMapFade >= 1) {
        for (auto& [index, texture] : gpu_->fadingFrom) {
            release(texture);
        }
        gpu_->fadingFrom.clear();
    }
}

void GlRenderer::setSkybox(const std::vector<scene::Image>& faces) {
    Texture fresh = uploadCube(faces);
    release(gpu_->skybox);
    gpu_->skybox = fresh;
}

void GlRenderer::releaseSprites() {
    for (auto& t : spriteTextures_) {
        release(t);
    }
    spriteTextures_.clear();
    spriteSizes_.clear();
}

void GlRenderer::setSprites(const std::vector<std::pair<int, const scene::Image*>>& images) {
    for (const auto& [index, image] : images) {
        if (index < 0 || image == nullptr || !complete(*image)) {
            continue;
        }
        const auto i = static_cast<std::size_t>(index);
        if (i >= spriteTextures_.size()) {
            spriteTextures_.resize(i + 1);
            spriteSizes_.resize(i + 1, {0, 0});
        }
        gl_.ActiveTexture(GL_TEXTURE0);
        // A new texture when the slot has none yet or the picture changed size, otherwise its pixels are replaced.
        Texture& t = spriteTextures_[i];
        if (t.id == 0 || spriteSizes_[i][0] != image->width || spriteSizes_[i][1] != image->height) {
            release(t);
            t = Texture{};
            gl_.GenTextures(1, &t.id);
            gl_.BindTexture(GL_TEXTURE_2D, t.id);
            gl_.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            gl_.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            gl_.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            gl_.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            t.wrap = GL_CLAMP_TO_EDGE;
            gl_.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
            gl_.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, static_cast<GLsizei>(image->width), static_cast<GLsizei>(image->height), 0, GL_RGBA,
                           GL_UNSIGNED_BYTE, image->rgba.data());
            spriteSizes_[i] = {image->width, image->height};
        } else {
            gl_.BindTexture(GL_TEXTURE_2D, t.id);
            gl_.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
            gl_.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, static_cast<GLsizei>(image->width), static_cast<GLsizei>(image->height), GL_RGBA,
                              GL_UNSIGNED_BYTE, image->rgba.data());
        }
    }
    bound_ = {};
}

bool GlRenderer::hasImage(int index) const {
    return index >= 0 && static_cast<std::size_t>(index) < gpu_->textures.size() && gpu_->textures[static_cast<std::size_t>(index)].id != 0;
}

bool GlRenderer::hasEnvCube(int index) const {
    return index >= 0 && static_cast<std::size_t>(index) < gpu_->envCubes.size() && gpu_->envCubes[static_cast<std::size_t>(index)].id != 0;
}

bool GlRenderer::hasFadeFrom(int index) const {
    const auto from = gpu_->fadingFrom.find(index);
    return from != gpu_->fadingFrom.end() && from->second.id != 0;
}

bool GlRenderer::skinReady(std::uint32_t buffers) const {
    return !gpu_->meshBuffers[buffers].cpuSkin.empty();
}

} // namespace ts2::render
