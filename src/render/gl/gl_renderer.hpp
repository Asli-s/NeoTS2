#pragma once

#include "render/frame_plan.hpp"
#include "render/gl/gl_api.hpp"
#include "render/irenderer.hpp"
#include "render/mesh_pack.hpp"
#include "render/shared_state.hpp"
#include "render/terrain_mesh.hpp"
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
        bool nearest = false;  // sampled without filtering
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

        // A neighborhood's painted terrain (see scene::PaintedTerrain). OpenGL 2 has no texture arrays:
        // each paint texture is its own, and the alpha maps and cells' textures of four paints share one.
        GLuint terrainVertices = 0, terrainIndices = 0;
        unsigned terrainIndexCount = 0;
        std::vector<Texture> terrainPaints;
        std::vector<std::uint8_t> terrainSlices;  // the paint textures some cell shows
        std::array<Texture, 2> terrainAlphas, terrainCells;
        Texture terrainLight, terrainGradient;
        TerrainUniforms terrainUniforms{};
        // Its roads, one texture each, drawn in runs of quads that share one.
        GLuint roadVertices = 0, roadIndices = 0;
        std::vector<Texture> roadTextures;
        std::vector<RoadMesh::Run> roadRuns;
        float lightMapSize = 1280;
        // Its lots' imposters.
        GLuint imposterVertices = 0, imposterIndices = 0;
        std::vector<Texture> imposterTextures;
        std::vector<ImposterMesh::Part> imposterDraws;
        // A lot skirt's boundary curtain.
        GLuint curtainVertices = 0, curtainIndices = 0;

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
        std::optional<scene::PaintedTerrain> terrain;
        std::optional<scene::Scene::BoundaryClip> clip;
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
        GLint vertex = -1, fragment = -1;  // the vertex and fragment shaders' constants
    };

    // Gathering and uploading (gl_upload.cpp).
    static std::unique_ptr<Staged> stage(const scene::Scene& scene);
    void uploadStaged(SceneGpu& gpu, Staged& staged);
    Texture uploadTexture(const scene::Image& image, bool mips, bool anisotropic = true);
    Texture uploadRaw(unsigned width, unsigned height, const std::uint8_t* rgba, bool nearest);
    GLuint uploadBuffer(GLenum target, const void* data, std::size_t bytes);
    void uploadTerrain(SceneGpu& gpu, const scene::PaintedTerrain& terrain);
    Texture uploadCube(const std::vector<scene::Image>& faces);
    MeshBuffers uploadMeshBuffers(MeshData& data);
    void release(Texture& texture);
    void releaseBuffer(GLuint& buffer);
    void collectGarbage();

    // Programs (gl_shaders.cpp).
    Program loadProgram(const std::filesystem::path& folder, const char* vertex, const char* fragment, const char* vertexBlock,
                        const char* fragmentBlock, std::initializer_list<const char*> samplers);
    GLuint compile(GLenum stage, const std::filesystem::path& file);
    void makePrograms(const std::filesystem::path& folder);

    // Frames (gl_draw.cpp).
    [[nodiscard]] bool render(unsigned width, unsigned height, const Camera& camera);
    void drawList(const FrameView& f, const std::vector<Draw>& list, int blend);
    void drawSprites(unsigned width, unsigned height);
    void bindTexture(unsigned unit, Texture& texture, GLint wrap, bool nearest = false);
    void drawSky(const FrameView& f, const Texture& cube, float up);
    void drawBoundary(const FrameView& f);
    void drawTerrain(const FrameView& f);
    void drawRoads(const FrameView& f);
    void drawImposters(const FrameView& f);
    // A draw's state: how it blends, whether it tests and writes depth (and how), and what it writes.
    struct State {
        int blend = 0;  // 0 none, 1 over, 2 add, 3 imposter light (multiply), 4 premultiplied over, 5 sum
        bool cull = false;
        bool depthTest = true;
        bool depthWrite = true;
        GLenum depthFunc = GL_GEQUAL;
        bool colorWrite = true;
        bool operator==(const State&) const = default;
    };
    void setState(const State& state);
    void attributes(unsigned location, int size, GLsizei stride, std::size_t offset);
    Texture& textureFor(const TextureRef& ref);
    void setAttributes(unsigned mask);
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

    Program scene_, ui_, terrain_, road_, imposter_, sky_, clip_;
    GLuint skyVertices_ = 0;  // the corners of the sky's triangle
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
    std::optional<State> state_;
    GLuint arrayBuffer_ = 0, elementBuffer_ = 0;
    std::array<std::pair<GLuint, GLint>, 8> bound_{};  // texture and how it is sampled (wrap, plus 0x10000 unfiltered) by unit

    // The off-screen target of drawOffscreen, remade when its size changes.
    GLuint targetFramebuffer_ = 0, targetColor_ = 0, targetDepth_ = 0;
    unsigned targetWidth_ = 0, targetHeight_ = 0;
    static constexpr unsigned kMaxOffscreen = 8192;

    std::unique_ptr<SceneGpu> gpu_ = std::make_unique<SceneGpu>();
};

} // namespace ts2::render
