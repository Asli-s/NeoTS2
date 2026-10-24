// Checks a fixed-function renderer's calls against what the console OpenGL libraries can do: GLdc
// (Dreamcast) and ps2gl (PlayStation 2). Each rule comes from those libraries' source, and says which of
// them it breaks. A call outside the subset still goes through (a PC runs it); it is reported once, so
// a PC build shows what a console build would have to do differently.
//
// It knows OpenGL's enums only as numbers, so nothing here depends on a graphics API.
#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace ts2::render {

class GlSubset {
public:
    enum Target : unsigned { GLdc = 1, Ps2gl = 2, Both = 3 };
    using Report = std::function<void(const std::string&)>;

    // `report` is given each distinct violation once.
    explicit GlSubset(Report report) : report_(std::move(report)) {}

    // glBlendFuncSeparate's four factors (GL_ZERO, GL_ONE, GL_SRC_ALPHA, ...).
    void blend(unsigned srcRgb, unsigned dstRgb, unsigned srcAlpha, unsigned dstAlpha);
    void alphaFunc(unsigned func);
    void texEnv(unsigned mode);
    // glEnable of a capability.
    void enable(unsigned capability);
    // A glColorMask that turns a channel off.
    void colorMaskOff();
    void drawElements(unsigned indexType);
    // Texture colours multiplied by more than the vertex colour makes them (GL_COMBINE with GL_RGB_SCALE).
    void overbright();
    // A vertex, colour or texture coordinate array (`name`), with the glXPointer arguments.
    void array(const char* name, int size, unsigned type, int stride);
    // More than one texture unit in use.
    void textureUnits(unsigned count);
    // A texture image of this target and size (a texture has no mip levels checked here), `what` being what it is
    // for ("image", "sprite", ...). A game has hundreds, so a rule they break is reported once, with the first as
    // its example, and counted (see summary).
    void texture(const char* what, unsigned target, unsigned width, unsigned height, unsigned bytesPerTexel = 4);

    // How much texture memory is in use now (RGBA bytes), by kind ("scene", "sprites"). The most they are
    // ever in use together is kept, and told at the end (see summary).
    void resident(const char* kind, std::size_t bytes);

    // How many distinct violations have been reported (a texture rule counts once).
    [[nodiscard]] std::size_t violations() const;
    // For each texture rule broken by more than one texture: how many broke it, how many of each kind, and the first.
    // Then the textures' memory, by kind, as the consoles' VRAM (4 MB on the PlayStation 2, 8 on the Dreamcast) would hold it.
    [[nodiscard]] std::vector<std::string> summary() const;

private:
    void violate(unsigned targets, const std::string& call, const std::string& what);
    // As violate, for a rule many calls break: only the first is reported, the rest are counted.
    void violateCounted(unsigned targets, const std::string& rule, const std::string& kind, const std::string& example);

    struct Counted {
        unsigned targets = 0;
        std::size_t count = 0;
        std::string example;
        std::map<std::string, std::size_t> kinds;
    };

    Report report_;
    mutable std::mutex lock_;  // textures are uploaded on other threads
    std::set<std::string> seen_;
    std::map<std::string, Counted> counted_;
    struct Memory {
        std::size_t textures = 0, bytes = 0;
    };
    std::map<std::string, Memory> memory_;  // by kind
    std::map<std::string, std::size_t> resident_, residentPeak_;  // by kind: now, and at the peak of the total
    std::size_t peakTotal_ = 0;
};

} // namespace ts2::render
