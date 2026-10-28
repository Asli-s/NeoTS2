#include "render/gl/gl_renderer.hpp"

#include "diag/log.hpp"
#include "diag/timing.hpp"

#include <algorithm>
#include <format>
#include <stdexcept>
#include <string>

namespace ts2::render {

GlRenderer::GlRenderer(platform::Window& window, const std::filesystem::path& shaderFolder) : window_(&window) {
    context_ = window_->createGlContext();
    if (context_ == nullptr) {
        diag::error(std::string("no OpenGL context: ") + platform::lastError());
        throw std::runtime_error(
            "NeoTS2 needs a graphics card and driver that support OpenGL 2.1, and none was found.\n\n"
            "Update your graphics driver and try again.");
    }
    // The destructor doesn't run when the constructor throws, so what was made is released here.
    try {
        window_->makeCurrent(context_.get());
        drawingThread_ = std::this_thread::get_id();
        // Presented with the display, as the SDL GPU renderer's swapchain is.
        window_->setSwapInterval(1);
        const char* missing = nullptr;
        if (!gl_.load(missing)) {
            throw std::runtime_error(std::string("this OpenGL driver lacks ") + missing);
        }
        if (gl_.majorVersion < 2 || (gl_.majorVersion == 2 && gl_.minorVersion < 1)) {
            throw std::runtime_error(std::format("NeoTS2 needs OpenGL 2.1, and this driver has {}.{}.", gl_.majorVersion, gl_.minorVersion));
        }
        const auto text = [&](GLenum name) {
            const auto* s = reinterpret_cast<const char*>(gl_.GetString(name));
            return std::string(s ? s : "unknown");
        };
        vendor_ = text(GL_VENDOR);
        renderer_ = text(GL_RENDERER);
        version_ = text(GL_VERSION);
        gl_.GenBuffers(1, &streamVertices_);
        gl_.GenBuffers(1, &spriteVertices_);
        // The sky is one triangle covering the screen, corners (0,0) (2,0) (0,2).
        const float corners[6] = {0, 0, 2, 0, 0, 2};
        skyVertices_ = uploadBuffer(GL_ARRAY_BUFFER, corners, sizeof(corners));
        invalidateState();
        makePrograms(shaderFolder);
        gpu_->white = uploadTexture({1, 1, {255, 255, 255, 255}, {}}, false);
        gpu_->blackCube = uploadCube(std::vector<scene::Image>(6, scene::Image{1, 1, {0, 0, 0, 255}, {}}));
        diag::info(std::format("OpenGL {}.{}: framebuffer objects {}, anisotropic filtering {}", gl_.majorVersion, gl_.minorVersion,
                               gl_.framebuffers ? "yes" : "no", gl_.anisotropy ? "yes" : "no"));
        makeSharedContexts(2);
        diag::info(pool_.empty() ? "OpenGL: contexts can't share objects here, so scenes are uploaded as they are drawn"
                                 : "OpenGL: scenes are uploaded on other threads in contexts that share objects");
    } catch (...) {
        destroySharedContexts();
        context_.reset();
        throw;
    }
}

GlRenderer::~GlRenderer() {
    if (context_ == nullptr) {
        return;
    }
    window_->makeCurrent(context_.get());
    destroySharedContexts();
    window_->makeCurrent(context_.get());
    gpu_->release(*graveyard_);
    releaseSprites();
    releaseTarget();
    releaseReflection();
    collectGarbage();
    releaseBuffer(streamVertices_);
    releaseBuffer(spriteVertices_);
    releaseBuffer(skyVertices_);
    for (const Program* p : {&scene_, &ui_, &terrain_, &road_, &imposter_, &sky_, &clip_, &water_}) {
        if (p->id != 0) {
            gl_.DeleteProgram(p->id);
        }
    }
    context_.reset();
}

Capabilities GlRenderer::capabilities() const {
    Capabilities c;
    c.waterReflection = gl_.framebuffers;
    c.gpuSkinning = false;
    c.projectedShadows = false;  // not drawn by this renderer yet
    c.anisotropic = gl_.anisotropy;
    c.threadedPrepare = !pool_.empty();
    c.offscreen = true;  // in a corner of the window when there are no framebuffer objects, which limits its size
    c.maxTextureSize = gl_.maxTextureSize;
    return c;
}

std::string GlRenderer::device() const {
    return std::format("{} {}, driver {} ({})", vendor_, renderer_, version_, backend());
}

void GlRenderer::bindFramebuffer(GLuint framebuffer) {
    if (gl_.framebuffers) {
        gl_.BindFramebufferEXT(GL_FRAMEBUFFER_EXT, framebuffer);
    }
    framebuffer_ = framebuffer;
}

void GlRenderer::bindWindow() {
    bindFramebuffer(0);
}

bool GlRenderer::draw(const Camera& camera) {
    int width = 0, height = 0;
    window_->sizeInPixels(width, height);
    if (width <= 0 || height <= 0 || window_->minimized()) {
        return false;
    }
    bindWindow();
    const bool drawn = render(static_cast<unsigned>(width), static_cast<unsigned>(height), camera);
    window_->swapBuffers();
    return drawn;
}

void GlRenderer::releaseTarget() {
    if (gl_.framebuffers) {
        if (targetFramebuffer_) gl_.DeleteFramebuffersEXT(1, &targetFramebuffer_);
        if (targetDepth_) gl_.DeleteRenderbuffersEXT(1, &targetDepth_);
    }
    if (targetColor_) {
        gl_.DeleteTextures(1, &targetColor_);
    }
    targetFramebuffer_ = targetColor_ = targetDepth_ = 0;
    targetWidth_ = targetHeight_ = 0;
}

// Draws one frame into an off-screen target of this size, which stays bound. False when there is no target.
bool GlRenderer::renderToTarget(const Camera& camera, unsigned width, unsigned height) {
    if (width == 0 || height == 0 || width > kMaxOffscreen || height > kMaxOffscreen || !gl_.framebuffers ||
        width > static_cast<unsigned>(gl_.maxTextureSize) || height > static_cast<unsigned>(gl_.maxTextureSize)) {
        return false;
    }
    if (targetFramebuffer_ == 0 || targetWidth_ != width || targetHeight_ != height) {
        releaseTarget();
        gl_.GenTextures(1, &targetColor_);
        gl_.ActiveTexture(GL_TEXTURE0);
        gl_.BindTexture(GL_TEXTURE_2D, targetColor_);
        bound_ = {};
        gl_.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        gl_.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        gl_.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, static_cast<GLsizei>(width), static_cast<GLsizei>(height), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        gl_.GenRenderbuffersEXT(1, &targetDepth_);
        gl_.BindRenderbufferEXT(GL_RENDERBUFFER_EXT, targetDepth_);
        gl_.RenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_DEPTH_COMPONENT24, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
        gl_.GenFramebuffersEXT(1, &targetFramebuffer_);
        bindFramebuffer(targetFramebuffer_);
        gl_.FramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, targetColor_, 0);
        gl_.FramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, targetDepth_);
        if (gl_.CheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT) != GL_FRAMEBUFFER_COMPLETE_EXT) {
            bindWindow();
            releaseTarget();
            return false;
        }
        targetWidth_ = width;
        targetHeight_ = height;
    } else {
        bindFramebuffer(targetFramebuffer_);
    }
    return render(width, height, camera);
}

// Draws one frame off-screen and reads it back as RGBA, rows from the top.
bool GlRenderer::drawOffscreen(const Camera& camera, unsigned width, unsigned height, scene::Image& out) {
    if (!gl_.framebuffers) {
        // With no framebuffer objects the picture is drawn in a corner of the window's back buffer, which
        // the next frame draws over. It can't be larger than the window.
        int windowWidth = 0, windowHeight = 0;
        window_->sizeInPixels(windowWidth, windowHeight);
        if (width == 0 || height == 0 || width > static_cast<unsigned>(windowWidth) || height > static_cast<unsigned>(windowHeight) ||
            !render(width, height, camera)) {
            return false;
        }
    } else if (!renderToTarget(camera, width, height)) {
        bindWindow();
        return false;
    }
    out.width = width;
    out.height = height;
    out.rgba.resize(std::size_t{width} * height * 4);
    gl_.PixelStorei(GL_PACK_ALIGNMENT, 1);
    gl_.ReadPixels(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height), GL_RGBA, GL_UNSIGNED_BYTE, out.rgba.data());
    bindWindow();
    // OpenGL reads from the bottom row up.
    const std::size_t row = std::size_t{width} * 4;
    for (std::size_t y = 0; y < height / 2; ++y) {
        std::swap_ranges(out.rgba.begin() + static_cast<std::ptrdiff_t>(y * row), out.rgba.begin() + static_cast<std::ptrdiff_t>((y + 1) * row),
                         out.rgba.begin() + static_cast<std::ptrdiff_t>((height - 1 - y) * row));
    }
    return true;
}

bool GlRenderer::drawToFile(const Camera& camera, unsigned width, unsigned height, const std::filesystem::path& path) {
    scene::Image image;
    if (!drawOffscreen(camera, width, height, image)) {
        return false;
    }
    return platform::savePng(path, image.rgba.data(), width, height);
}

std::optional<scene::Image> GlRenderer::drawInstancesToImage(const Camera& camera, unsigned width, unsigned height,
                                                             const scene::ObjectLighting& lighting) {
    // Back to the scene's own drawing afterwards, even if drawing throws.
    struct Restore {
        GlRenderer& r;
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

void GlRenderer::benchmark(const Camera& camera, unsigned width, unsigned height, int frames) {
    const bool wasOn = diag::timingsOn();
    // The first frames warm caches up and aren't counted.
    for (int f = -10; f < frames; ++f) {
        diag::setTimings(f >= 0);
        diag::Timer timer("gpu frame");
        if (gl_.framebuffers ? !renderToTarget(camera, width, height) : !render(width, height, camera)) {
            break;
        }
        gl_.Finish();
    }
    bindWindow();
    diag::setTimings(wasOn);
}

} // namespace ts2::render
