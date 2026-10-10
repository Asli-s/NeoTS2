// The game's main menu (the neighborhood chooser), built from its UI script.
#include "menu.hpp"

#include "content/neighborhood.hpp"
#include "content/ui_data.hpp"
#include "formats/image.hpp"
#include "formats/reia.hpp"
#include "render/renderer.hpp"
#include "ui/fonts.hpp"
#include "ui/screen.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <future>
#include <random>
#include <stdexcept>
#include <string>

namespace {

constexpr std::uint32_t kScripts = 0xA99D8A11;
constexpr std::uint32_t kMainMenu = 0x49001017;
constexpr std::uint32_t kNeighborhoodTile = 0x49001018;
// Windows of the main menu by id.
constexpr std::uint32_t kBackdrop = 0x0da36c7d, kThumbs = 0x00001006, kQuit = 0x000000a5, kPrevious = 0x00001004, kNext = 0x00001005;
// Windows of a neighborhood tile by id.
constexpr std::uint32_t kTilePicture = 0x000000a1, kTileName = 0x000000a5;
constexpr std::uint32_t kTileButton = 0x000000a2;
// The tile's tooltip, which the game sets itself: "Click for Preview of Neighborhood".
constexpr std::uint32_t kNeighborhoodText = 0x7F3A262A, kTileTip = 0x0086003F;  // a string id, leakcheck: ok
// The neighborhood preview and its windows by id.
constexpr std::uint32_t kPreview = 0x49001021;
constexpr std::uint32_t kPreviewPicture = 0x00002001, kPreviewDescription = 0x00002002, kPreviewName = 0x00002003, kPreviewPlay = 0x000000b2,
                        kPreviewClose = 0x000000b3;
constexpr std::uint32_t kConstants = 0x4dc1dce2, kUpperLeftSim = 0x000000e1, kLowerRightSim = 0x000000e3;
constexpr std::uint32_t kImages = 0x499DB772;
constexpr int kPerPage = 3;
constexpr int kTileGap = 4;  // between neighborhood tiles

std::optional<ts2::tga::Pixels> readImage(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    return bytes.empty() ? std::nullopt : ts2::image::decode(bytes);
}

std::vector<ts2::render::Sprite> sprites(const ts2::ui::Screen& screen, double seconds) {
    std::vector<ts2::render::Sprite> out;
    for (const auto& q : screen.quads(seconds)) {
        ts2::render::Sprite s;
        s.x0 = q.x0;
        s.y0 = q.y0;
        s.x1 = q.x1;
        s.y1 = q.y1;
        s.u0 = q.u0;
        s.v0 = q.v0;
        s.u1 = q.u1;
        s.v1 = q.v1;
        s.texture = q.texture;
        for (int k = 0; k < 4; ++k) {
            s.color[k] = q.color[static_cast<std::size_t>(k)];
        }
        out.push_back(s);
    }
    return out;
}


} // namespace

int runMenu(const std::filesystem::path& install, const std::filesystem::path& shaders, const MenuOptions& options) {
    std::printf("loading the game's UI...\n");
    ts2::content::UiData data(install);
    ts2::ui::Fonts fonts(install);
    std::printf("%zu UI and text packages, %zu font faces\n", data.packageCount(), fonts.faceCount());

    const int width = options.width, height = options.height;
    SDL_Window* window = SDL_CreateWindow("NeoTS2", width, height, options.shot.empty() ? 0 : SDL_WINDOW_HIDDEN);
    if (window == nullptr) {
        throw std::runtime_error(SDL_GetError());
    }
    {
        ts2::render::Renderer renderer(window, shaders);
        // What shows around the menu: the game's dark blue (as captured from the game).
        renderer.setClearColor(1 / 255.f, 35 / 255.f, 62 / 255.f);
        ts2::ui::Screen screen(data, fonts);
        // Placed in the middle of the screen (as captured from the game at 1920 x 1080).
        const int menu = screen.load(kScripts, kMainMenu, 0, 0);
        if (menu < 0) {
            throw std::runtime_error("the main menu's UI script is missing");
        }
        const int mx = (width - screen.width(menu)) / 2, my = (height - screen.height(menu)) / 2;
        screen.move(menu, mx, my);
        // At start the blurred neighborhood picture shows behind the menu, in the middle of the
        // screen (as captured).
        const int backdrop = screen.find(menu, kBackdrop);
        screen.setVisible(backdrop, true);
        screen.move(backdrop, (width - screen.width(backdrop)) / 2 - mx, (height - screen.height(backdrop)) / 2 - my);
        // A Sim in the upper left and one in the lower right, each one of the pictures listed in the
        // menu's table of constants (kUpperLeft, kLowerRight); which one the game picks is not traced.
        std::mt19937 random(static_cast<unsigned>(SDL_GetTicksNS()));
        for (const auto& line : screen.childTexts(screen.find(menu, kConstants))) {
            const auto eq = line.find('=');
            if (eq == std::string::npos) {
                continue;
            }
            const std::string name = line.substr(0, eq);
            const std::uint32_t target = name == "kUpperLeft" ? kUpperLeftSim : name == "kLowerRight" ? kLowerRightSim : 0;
            std::vector<std::uint32_t> pictures;
            for (std::size_t at = eq + 1; at < line.size();) {
                const auto end = line.find(';', at);
                const std::string id = line.substr(at, end == std::string::npos ? std::string::npos : end - at);
                if (!id.empty()) {
                    pictures.push_back(static_cast<std::uint32_t>(std::strtoul(id.c_str(), nullptr, 16)));
                }
                at = end == std::string::npos ? line.size() : end + 1;
            }
            if (target == 0 || pictures.empty()) {
                continue;
            }
            if (auto picture = data.image(kImages, pictures[random() % pictures.size()])) {
                screen.setImage(screen.find(menu, target), std::move(*picture));
            }
        }
        // A tile per neighborhood in the thumbnail strip, three to a page, 4 apart, the row centered in
        // the strip both ways (as captured from the game).
        const auto hoods = ts2::content::listNeighborhoods(options.user / "Neighborhoods");
        std::printf("%zu neighborhoods\n", hoods.size());
        const int thumbs = screen.find(menu, kThumbs);
        std::vector<int> tiles;
        for (int i = 0; i < kPerPage; ++i) {
            tiles.push_back(screen.load(kScripts, kNeighborhoodTile, 0, 0, thumbs));
        }
        const int tileWidth = screen.width(tiles[0]), tileHeight = screen.height(tiles[0]);
        const int left = (screen.width(thumbs) - (kPerPage * tileWidth + (kPerPage - 1) * kTileGap)) / 2;
        for (int i = 0; i < kPerPage; ++i) {
            screen.move(tiles[static_cast<std::size_t>(i)], left + i * (tileWidth + kTileGap), (screen.height(thumbs) - tileHeight) / 2);
        }
        // Each tile plays its neighborhood's moving picture (<hood>_Neighborhood.reia) over and over,
        // from its first frame when the page is shown (not traced).
        // The videos are read in the background as the menu opens and kept, so a page shows at once.
        std::vector<std::shared_future<std::vector<std::uint8_t>>> videos;
        for (const auto& hood : hoods) {
            const auto path = hood.folder / (hood.folder.filename().u8string() + u8"_Neighborhood.reia");
            videos.push_back(std::async(std::launch::async, [path] {
                std::ifstream in(path, std::ios::binary);
                return std::vector<std::uint8_t>{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
            }).share());
        }
        struct Playing {
            ts2::reia::Video video;
            ts2::tga::Pixels picture;
            std::size_t frame = 0;
            int window = -1;
        };
        std::vector<Playing> playing(kPerPage);
        int page = 0;
        const int pages = std::max(1, (static_cast<int>(hoods.size()) + kPerPage - 1) / kPerPage);
        Uint64 pageShown = SDL_GetTicksNS();
        auto showPage = [&] {
            for (int i = 0; i < kPerPage; ++i) {
                const std::size_t n = static_cast<std::size_t>(page * kPerPage + i);
                const int tile = tiles[static_cast<std::size_t>(i)];
                Playing& p = playing[static_cast<std::size_t>(i)];
                p = {};
                screen.setVisible(tile, n < hoods.size());
                if (n >= hoods.size()) {
                    continue;
                }
                screen.setText(screen.find(tile, kTileName), hoods[n].name);
                screen.setTip(screen.find(tile, kTileButton), data.text(kNeighborhoodText, kTileTip));
                p.window = screen.find(tile, kTilePicture);
                p.video = ts2::reia::parse(videos[n].get());
                if (!p.video.frames.empty() && ts2::reia::decodeFrame(p.video, 0, p.picture)) {
                    screen.setImage(p.window, p.picture);
                } else if (auto picture = readImage(hoods[n].thumbnail)) {
                    screen.setImage(p.window, std::move(*picture));
                }
            }
            screen.setVisible(screen.find(menu, kPrevious), page > 0);
            screen.setVisible(screen.find(menu, kNext), page + 1 < pages);
            pageShown = SDL_GetTicksNS();
        };
        showPage();

        // A neighborhood's preview, over the menu where the menu is: its name, description and
        // picture, and Play.
        const int preview = screen.load(kScripts, kPreview, mx, my);
        screen.setVisible(preview, false);
        std::size_t chosen = 0;
        auto showPreview = [&] {
            const auto& hood = hoods[chosen];
            screen.setText(screen.find(preview, kPreviewName), hood.name);
            screen.setText(screen.find(preview, kPreviewDescription), hood.description);
            if (auto picture = readImage(hood.thumbnail)) {
                screen.setImage(screen.find(preview, kPreviewPicture), std::move(*picture));
            }
            screen.setVisible(preview, true);
            screen.setModal(preview);
        };
        auto play = [&] {
            const double seconds = static_cast<double>(SDL_GetTicksNS() - pageShown) / 1e9;
            for (auto& p : playing) {
                if (p.video.frames.empty()) {
                    continue;
                }
                const auto count = p.video.frames.size();
                const auto at = static_cast<std::size_t>(seconds * p.video.rateNumerator / p.video.rateDenominator) % count;
                if (at == p.frame) {
                    continue;
                }
                if (at < p.frame) {  // round again: frames build on the ones before
                    p.picture = {};
                    p.frame = 0;
                    ts2::reia::decodeFrame(p.video, 0, p.picture);
                }
                while (p.frame < at) {
                    ts2::reia::decodeFrame(p.video, ++p.frame, p.picture);
                }
                screen.setImage(p.window, p.picture);
            }
        };

        // Only textures added or changed since the last frame go to the GPU.
        std::vector<ts2::scene::Image> staged;
        auto frame = [&] {
            const auto changed = screen.takeChangedTextures();
            staged.clear();
            staged.reserve(changed.size());
            std::vector<std::pair<int, const ts2::scene::Image*>> upload;
            for (const int t : changed) {
                const auto& px = screen.textures()[static_cast<std::size_t>(t)];
                staged.push_back({px.width, px.height, px.rgba});
            }
            for (std::size_t i = 0; i < changed.size(); ++i) {
                upload.emplace_back(changed[i], &staged[i]);
            }
            renderer.setSprites(upload);
            renderer.showSprites(sprites(screen, static_cast<double>(SDL_GetTicksNS()) / 1e9), false);
        };
        if (options.preview >= 0 && static_cast<std::size_t>(options.preview) < hoods.size()) {
            chosen = static_cast<std::size_t>(options.preview);
            showPreview();
        }
        if (options.hoverX >= 0) {
            screen.mouse(options.hoverX, options.hoverY, false, 0);
        }
        if (!options.shot.empty()) {
            frame();
            renderer.showSprites(sprites(screen, options.hoverX >= 0 ? 10.0 : 0.0), false);
            if (!renderer.drawToFile({}, static_cast<unsigned>(width), static_cast<unsigned>(height), options.shot)) {
                throw std::runtime_error("couldn't save the picture");
            }
            std::printf("wrote %s\n", options.shot.string().c_str());
        }
        bool running = options.shot.empty();
        bool down = false;
        while (running) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                if (e.type == SDL_EVENT_QUIT || (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE)) {
                    running = false;
                } else if (e.type == SDL_EVENT_MOUSE_MOTION || e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP) {
                    float x = 0, y = 0;
                    SDL_GetMouseState(&x, &y);
                    if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) {
                        down = true;
                    } else if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_LEFT) {
                        down = false;
                    }
                    if (const auto clicked = screen.mouse(static_cast<int>(x), static_cast<int>(y), down, static_cast<double>(SDL_GetTicksNS()) / 1e9)) {
                        const std::uint32_t id = screen.id(*clicked);
                        std::printf("clicked 0x%08x\n", id);
                        const auto tile = std::find(tiles.begin(), tiles.end(), screen.parent(*clicked));
                        if (screen.visible(preview)) {
                            if (id == kPreviewClose) {
                                screen.setVisible(preview, false);
                                screen.setModal(-1);
                            } else if (id == kPreviewPlay) {
                                std::printf("play %s\n", hoods[chosen].folder.filename().string().c_str());
                            }
                        } else if (id == kQuit) {
                            running = false;
                        } else if (id == kNext && page + 1 < pages) {
                            ++page;
                            showPage();
                        } else if (id == kPrevious && page > 0) {
                            --page;
                            showPage();
                        } else if (id == kTileButton && tile != tiles.end()) {
                            chosen = static_cast<std::size_t>(page * kPerPage + (tile - tiles.begin()));
                            showPreview();
                        }
                    }
                }
            }
            play();
            frame();
            if (!renderer.draw({})) {
                SDL_Delay(16);
            }
        }
    }
    SDL_DestroyWindow(window);
    return 0;
}
