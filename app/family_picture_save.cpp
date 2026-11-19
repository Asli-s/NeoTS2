#include "game.hpp"

#include "formats/dbpf_writer.hpp"
#include "formats/image.hpp"
#include "diag/log.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>

namespace {

// Family pictures: Thumbnails/<hood>_FamilyThumbnails.package, one per family by its id.
constexpr std::uint32_t kFamilyPictureType = 0x8C3CE95A;
constexpr std::uint32_t kLocalGroup = 0xFFFFFFFF;

} // namespace

namespace app {

std::optional<ts2::tga::Pixels> Game::drawFamilyPicture() {
    const auto arrangement = lotLife_->familyPicture();
    if (!arrangement) {
        return std::nullopt;
    }
    if (!arrangement->unknown.empty()) {
        ts2::diag::warning("save: the household's family picture is not drawn, as it needs a rule not known yet: " + arrangement->unknown);
        return std::nullopt;
    }
    const auto stage = lotLife_->familyPictureStage(arrangement->camera);
    if (!stage) {
        ts2::diag::warning("save: the family picture's stage can't be read from the game's files");
        return std::nullopt;
    }
    std::vector<FamilyPictureActor> actors;
    for (const auto& place : arrangement->sims) {
        // A slot the stage doesn't have leaves its Sim out, as the game leaves it.
        const auto slot = stage->slots.find(place.slot);
        if (slot == stage->slots.end()) {
            continue;
        }
        if (auto actor = lotLife_->addFamilyPictureSim(lot_, place.id, slot->second)) {
            constexpr std::string_view kAnimSuffix = "_anim";
            std::string pose = place.animation;
            if (pose.ends_with(kAnimSuffix)) {
                pose.resize(pose.size() - kAnimSuffix.size());
            }
            actors.push_back({std::move(*actor), std::move(pose)});
        }
    }
    const auto send = [&] {
        for (const auto& a : actors) {
            for (const auto& s : a.actor.skinned) {
                renderer_.setInstance(lot_, s.instance);
            }
            for (const auto& h : a.actor.held) {
                renderer_.setInstance(lot_, h.instance);
            }
        }
    };
    renderer_.addMaterials(lot_);
    renderer_.addMeshes(lot_);
    send();
    auto picture = app::drawFamilyPicture(lot_, renderer_, lotLife_->animations(), actors, *stage);
    // The copies leave the lot at once, so they are never drawn there.
    for (const auto& a : actors) {
        lotLife_->removePictureSim(lot_, a.actor);
    }
    send();
    renderer_.show(lotShown());
    return picture;
}

std::optional<std::pair<std::filesystem::path, std::vector<std::uint8_t>>> Game::familyPictureFile() {
    const auto family = lotLife_->simulation().world().lotHousehold();
    if (!family) {
        return std::nullopt;
    }
    // The game makes each neighborhood's package at the main menu, so one that is missing is left so.
    const auto path = openFolder_ / "Thumbnails" / (openFolder_.filename().u8string() + u8"_FamilyThumbnails.package");
    std::vector<std::uint8_t> file;
    {
        std::ifstream in(path, std::ios::binary);
        file.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    if (file.empty()) {
        ts2::diag::warning("save: the neighborhood has no family pictures package, so the household's picture is not written");
        return std::nullopt;
    }
    const auto picture = drawFamilyPicture();
    if (!picture) {
        return std::nullopt;
    }
    const std::vector<std::uint8_t> jpeg = ts2::image::encodeJpegWithAlpha(*picture);
    if (jpeg.empty()) {
        ts2::diag::warning("save: the household's family picture can't be encoded");
        return std::nullopt;
    }
    ts2::dbpf::PackageImage package = ts2::dbpf::readImage(file);
    const ts2::dbpf::ResourceKey key{kFamilyPictureType, kLocalGroup, *family, 0};
    auto stored = ts2::dbpf::storeResource(key, jpeg);
    const auto same = std::find_if(package.resources.begin(), package.resources.end(), [&](const auto& r) { return r.key == key; });
    if (same != package.resources.end()) {
        *same = std::move(stored);
    } else {
        package.dataOrder.push_back(package.resources.size());
        package.resources.push_back(std::move(stored));
    }
    return std::pair{path, ts2::dbpf::writeImage(package)};
}

} // namespace app
