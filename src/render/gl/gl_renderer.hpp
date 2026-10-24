#pragma once

#include "render/frame_plan.hpp"
#include "render/gl/gl_api.hpp"
#include "render/irenderer.hpp"
#include "render/mesh_pack.hpp"
#include "render/shared_state.hpp"
#include "scene/scene.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

struct SDL_Window;
typedef struct SDL_GLContextState* SDL_GLContext;

namespace ts2::render {

// Draws a Scene with OpenGL 2.1 (and a few common extensions), for systems that have no more.
// Shaders are GLSL 1.20 made from the same HLSL the SDL GPU renderer uses (see CMakeLists.txt).
//
// OpenGL has one context, which only the thread that draws may use, so prepare() only gathers what
// to upload and swap() uploads it. Everything else that touches the GPU is on the drawing thread too.
class GlRenderer final : public RendererState, private ResourceQuery {
public:
    GlRenderer(SDL_Window* window, const std::filesystem::path& shaderFolder);
    ~GlRenderer() override;

    using IRenderer::upload;
    [[nodiscard]] Capabilities capabilities() const override;
    [[nodiscard]] Prepared prepare(const scene::Scene& scene) const override;
    void upload(Prepared prepared) override;
    [[nodiscard]] Prepared swap(Prepared prepared) override;
    [[nodiscard]] bool uploaded() override { return true; }
    void setSprites(const std::vector<std::pair<int, const scene::Image*>>& images) override;
    [[nodiscard]] bool draw(const Camera& camera) override;
    [[nodiscard]] bool drawToFile(const Camera& camera, unsigned width, unsigned height, const std::filesystem::path& path) override;
    [[nodiscard]] std::optional<scene::Image> drawInstancesToImage(const Camera& camera, unsigned width, unsigned height,
                                                                   const scene::ObjectLighting& lighting) override;
    std::string backend() const override { return "opengl"; }
    std::string device() const override;
    void benchmark(const Camera& camera, unsigned width, unsigned height, int frames) override;
    void addMaterials(const scene::Scene& scene) override;
    void addMeshes(const scene::Scene& scene) override;
    void setImages(const scene::Scene& scene, const std::vector<int>& indexes, bool fade = false) override;
    void setLightMapFade(float fade) override;
    void setSkybox(const std::vector<scene::Image>& faces) override;

private:
    // What the frame plan asks about the uploaded scene (see ResourceQuery).
    const std::vector<GpuMesh>& meshes() const override { return gpu_->meshes; }
    bool hasImage(int index) const override;
    bool hasEnvCube(int index) const override;
    bool hasFadeFrom(int index) const override;
    float lightMapFade() const override { return gpu_->lightMapFade; }
    bool skinReady(std::uint32_t buffers) const override;

    // A texture object and how it is sampled right now. A GL 2 texture carries its sampler's state,
    // so the wrap mode is set when a draw needs another than the last one used.
    struct Texture {
        GLuint id = 0;
        GLenum target = GL_TEXTURE_2D;
        bool mips = false;
        GLint wrap = GL_REPEAT;
    };
    // Objects to delete on the drawing thread: a prepared scene can be let go of on any thread.
    struct Graveyard {
        std::mutex lock;
        std::vector<GLuint> textures, buffers;
    };
    // One vertex and one index buffer: the uploaded scene's meshes, then each batch of meshes added
    // later (see addMeshes). Skinned primitives are posed on the CPU from `vertices` and `skin`.
    struct MeshBuffers {
        GLuint vertices = 0;
        GLuint indices = 0;
        std::vector<float> cpuVertices;
        std::vector<std::uint8_t> cpuSkin;
    };
    // What a scene is drawn with on the GPU.
    struct SceneGpu {
        std::vector<Texture> textures;  // id 0 where an image is broken
        Texture white;                  // stands in for surfaces without a texture
        std::vector<GpuMesh> meshes;
        std::vector<MeshBuffers> meshBuffers;
        std::vector<Texture> envCubes;
        Texture blackCube;
        Texture skybox;  // a lot's sky
        // Light maps being faded from, by the image they are replacing (see setImages), and how far
        // the fade has gone (0 .. 1).
        std::map<int, Texture> fadingFrom;
        float lightMapFade = 1;

        void release(Graveyard& graveyard);
        // Releases up to `count` textures. True when none are left.
        bool releaseTextures(Graveyard& graveyard, std::size_t count);
    };
    // What prepare gathers on any thread, for swap to upload on the drawing thread.
    struct Staged {
        std::vector<scene::Image> images;
        std::set<std::size_t> noMips;  // light maps, drawn without smaller levels
        std::vector<GpuMesh> meshes;
        MeshData meshData;
        std::vector<scene::Image> skybox;
        std::vector<std::vector<scene::Image>> envCubes;
    };
    // A scene prepared: staged until swap uploads it, then on the GPU.
    struct GpuPrepared final : PreparedBase {
        ~GpuPrepared() override { release(); }
        void releaseSome(std::size_t textures) override;
        void release();
        std::shared_ptr<Graveyard> graveyard;
        std::unique_ptr<Staged> staged;
        std::unique_ptr<SceneGpu> gpu;
    };

    // A shader program and where its inputs are.
    struct Program {
        GLuint id = 0;
        GLint frame = -1, material = -1, screen = -1;
    };

    // Gathering and uploading (gl_upload.cpp).
    static std::unique_ptr<Staged> stage(const scene::Scene& scene);
    void uploadStaged(SceneGpu& gpu, Staged& staged);
    Texture uploadTexture(const scene::Image& image, bool mips);
    Texture uploadCube(const std::vector<scene::Image>& faces);
    MeshBuffers uploadMeshBuffers(MeshData& data);
    void release(Texture& texture);
    void releaseBuffer(GLuint& buffer);
    void collectGarbage();

    // Programs (gl_shaders.cpp).
    Program loadProgram(const std::filesystem::path& folder, const char* vertex, const char* fragment);
    GLuint compile(GLenum stage, const std::filesystem::path& file);
    void makePrograms(const std::filesystem::path& folder);

    // Frames (gl_draw.cpp).
    [[nodiscard]] bool render(unsigned width, unsigned height, const Camera& camera);
    void drawList(const FrameView& f, const std::vector<Draw>& list, int blend);
    void drawSprites(unsigned width, unsigned height);
    void bindTexture(unsigned unit, Texture& texture, GLint wrap);
    Texture& textureFor(const TextureRef& ref);
    void setAttributes(unsigned mask);
    void setBlend(int kind);
    void setCull(bool on);
    void setDepthWrite(bool on);
    void useProgram(const Program& program);
    // Poses a skinned primitive's vertices on the CPU into the streaming buffer.
    void poseSkinned(const GpuPrimitive& p, const MeshBuffers& buffers, std::size_t instance);
    [[nodiscard]] bool drawOffscreen(const Camera& camera, unsigned width, unsigned height, scene::Image& out);
    // Draws one frame into the off-screen target (made at that size), left bound.
    [[nodiscard]] bool renderToTarget(const Camera& camera, unsigned width, unsigned height);
    void bindWindow();
    void invalidateState();
    void releaseTarget();

    SDL_Window* window_ = nullptr;
    SDL_GLContext context_ = nullptr;
    gl::Api gl_;
    std::shared_ptr<Graveyard> graveyard_ = std::make_shared<Graveyard>();
    std::string vendor_, renderer_, version_;

    Program scene_, ui_;
    GLuint streamVertices_ = 0;  // a posed skinned primitive's vertices
    GLuint spriteVertices_ = 0;
    std::size_t streamCapacity_ = 0, spriteCapacity_ = 0;  // bytes

    std::vector<Texture> spriteTextures_;
    std::vector<float> scratch_;  // what is sent to a streaming buffer
    std::vector<std::array<unsigned, 2>> spriteSizes_;
    void releaseSprites();

    // What is set in the GL state now, so that draws only change what they must.
    GLuint program_ = 0;
    unsigned attributeMask_ = 0;
    int blend_ = -1;
    int cull_ = -1;
    int depthWrite_ = -1;
    GLuint arrayBuffer_ = 0, elementBuffer_ = 0;
    std::array<std::pair<GLuint, GLint>, 8> bound_{};  // texture and wrap by unit

    // The off-screen target of drawOffscreen, remade when its size changes.
    GLuint targetFramebuffer_ = 0, targetColor_ = 0, targetDepth_ = 0;
    unsigned targetWidth_ = 0, targetHeight_ = 0;
    static constexpr unsigned kMaxOffscreen = 8192;

    std::unique_ptr<SceneGpu> gpu_ = std::make_unique<SceneGpu>();
};

} // namespace ts2::render
