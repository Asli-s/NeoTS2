// Checks a fixed-function renderer's calls against what the console OpenGL libraries can do: GLdc
// (Dreamcast) and ps2gl (PlayStation 2). Each rule comes from those libraries' source, and says which of
// them it breaks. A call outside the subset still goes through (a PC runs it); it is reported once, so
// a PC build shows what a console build would have to do differently.
//
// It knows OpenGL's enums only as numbers, so nothing here depends on a graphics API.
#pragma once

#include <functional>
#include <mutex>
#include <set>
#include <string>

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
    // A vertex, colour or texture coordinate array (`name`), with the glXPointer arguments.
    void array(const char* name, int size, unsigned type, int stride);
    // More than one texture unit in use.
    void textureUnits(unsigned count);
    // A texture image of this target and size (a texture has no mip levels checked here).
    void texture(unsigned target, unsigned width, unsigned height);

    // How many distinct violations have been reported.
    [[nodiscard]] std::size_t violations() const;

private:
    void violate(unsigned targets, const std::string& call, const std::string& what);

    Report report_;
    mutable std::mutex lock_;  // textures are uploaded on other threads
    std::set<std::string> seen_;
};

} // namespace ts2::render
