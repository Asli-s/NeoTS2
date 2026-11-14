// Build mode's terrain paint brush: paints joining a lot's list, painting and rubbing out.
#include "test_support.hpp"

#include "content/terrain_paint.hpp"

namespace {

std::uint8_t strength(const ts2::content::Lot& lot, std::size_t paint, int i, int j) {
    return lot.flatGrids.at(ts2::content::kFirstPaintMapInstance + static_cast<std::uint32_t>(paint)).byte(i, j);
}

// A 4 x 4 tile lot: its maps are 17 points each way.
ts2::content::Lot plainLot() {
    ts2::content::Lot lot;
    lot.terrainTextures = ts2::terrain::LotTexture{};
    lot.terrainTextures->width = lot.terrainTextures->height = 4;
    lot.terrainTextures->canvas = "lottexture-canvas";
    return lot;
}

// A paint joins the list once, with an empty map the lot's size.
void paintsJoinTheLotsList() {
    auto lot = plainLot();
    CHECK(ts2::content::terrainPaintIndex(lot, "terrain-grass-a-paint") == std::optional<std::size_t>(0));
    CHECK(ts2::content::terrainPaintIndex(lot, "terrain-cover-a-paint") == std::optional<std::size_t>(1));
    CHECK(ts2::content::terrainPaintIndex(lot, "terrain-grass-a-paint") == std::optional<std::size_t>(0));
    const auto& map = lot.flatGrids.at(ts2::content::kFirstPaintMapInstance + 1);
    CHECK(map.width == 17 && map.height == 17 && map.elements.size() == 17 * 17 && strength(lot, 1, 8, 8) == 0);
    ts2::content::Lot bare;
    CHECK(!ts2::content::terrainPaintIndex(bare, "terrain-grass-a-paint"));
}

// The brush paints its paint in full near its middle, fading out to its edge, over the others; with
// no paint it rubs them all out.
void theBrushPaintsOverOthers() {
    auto lot = plainLot();
    const auto grass = ts2::content::terrainPaintIndex(lot, "grass");
    const auto dirt = ts2::content::terrainPaintIndex(lot, "dirt");
    CHECK(ts2::content::paintTerrain(lot, grass, 2.0f, 2.0f, 1.0f));
    CHECK(strength(lot, 0, 8, 8) == 255 && strength(lot, 0, 10, 8) == 255);  // half a tile out: within 60%
    CHECK(strength(lot, 0, 11, 8) > 0 && strength(lot, 0, 11, 8) < 255);   // three quarters out: fading
    CHECK(strength(lot, 0, 12, 8) == 0 && strength(lot, 0, 0, 0) == 0);    // a tile out: past the edge
    CHECK(ts2::content::paintTerrain(lot, dirt, 2.0f, 2.0f, 0.5f));
    CHECK(strength(lot, 1, 8, 8) == 255 && strength(lot, 0, 8, 8) == 0 && strength(lot, 0, 10, 8) == 255);
    // The same again changes nothing.
    CHECK(!ts2::content::paintTerrain(lot, dirt, 2.0f, 2.0f, 0.5f));
    // Rubbed out.
    CHECK(ts2::content::paintTerrain(lot, std::nullopt, 2.0f, 2.0f, 1.0f));
    CHECK(strength(lot, 1, 8, 8) == 0 && strength(lot, 0, 10, 8) == 0);
    // Off the lot's edge it keeps to the maps.
    CHECK(ts2::content::paintTerrain(lot, grass, 0.0f, 0.0f, 1.0f) && strength(lot, 0, 0, 0) == 255);
}

} // namespace

std::vector<Test> terrainPaintTests() {
    return {
        {"paints join the lot's list", paintsJoinTheLotsList},
        {"the brush paints over others", theBrushPaintsOverOthers},
    };
}
