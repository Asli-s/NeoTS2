// Asset loading and model building, from hand-made packages (see test_resources.hpp).
#include "test_support.hpp"
#include "test_resources.hpp"

#include "formats/dbpf.hpp"
#include "scene/age_data.hpp"
#include "scene/assets.hpp"
#include "scene/builder.hpp"
#include "scene/face_blend.hpp"
#include "scene/lot_skirt.hpp"
#include "scene/outfit_catalog.hpp"
#include "scene/sims.hpp"

#include <bit>
#include <cmath>
#include <filesystem>
#include <memory>

namespace {

namespace sg = ts2::scenegraph;

// The hand-made resources read back through the readers the game's files go through.
void handMadeResourcesReadBack() {
    const auto resources = testres::squareModel("square");
    const auto bytes = testres::package(resources);
    ts2::dbpf::Package package(bytes);
    CHECK(package.entries().size() == resources.size());
    auto read = [&](const sg::Link& key) {
        for (const auto& e : package.entries()) {
            if (e.key.type == key.type && e.key.group == key.group && e.key.instance == key.instance && e.key.instanceHigh == key.instanceHigh) {
                return package.read(e);
            }
        }
        return std::vector<std::uint8_t>{};
    };
    const auto shape = sg::parseShape(read(resources[1].key));
    CHECK(shape.geometryNodes.size() == 1 && shape.geometryNodes[0] == "square_tslocator_gmnd");
    CHECK(shape.parts.size() == 1 && shape.parts[0].group == "square" && shape.parts[0].material == "square_material");
    const auto node = sg::parseContainer(read(resources[2].key));
    CHECK(node.links.size() == 1 && node.links[0].type == testres::kGmdc);
    const auto mesh = sg::parseMesh(read(resources[3].key));
    CHECK(mesh.version == 4 && mesh.groups.size() == 1 && mesh.groups[0].indices.size() == 6);
    CHECK(mesh.links.size() == 1 && sg::vertexFloats(mesh, mesh.links[0], sg::kPositions).size() == 12);
    CHECK(sg::vertexFloats(mesh, mesh.links[0], sg::kTextureCoordinates).size() == 8);
    const auto material = sg::parseMaterial(read(resources[4].key));
    CHECK(material.type == "StandardMaterial" && material.properties.at("stdMatBaseTextureName") == "square_texture");
    const auto image = sg::parseImage(read(resources[5].key));
    CHECK(image.width == 2 && image.height == 2 && image.format == 1 && image.mips.size() == 1);
    const auto rgba = sg::decodeRgba(image.mips[0].data, 2, 2, image.format);
    CHECK(rgba.size() == 16 && rgba[0] == 255 && rgba[1] == 0 && rgba[5] == 255 && rgba[10] == 255 && rgba[12] == 255);
}

// Textures and other scenegraph resources are found by name in the game folder's model packages.
void assetsFindResourcesByName() {
    const auto root = testres::gameFolder("neots2-assets-test", testres::squareModel("square"));
    {
        ts2::scene::Assets assets(root);
        const auto texture = assets.texture("square_texture");
        CHECK(texture && texture->width == 2 && texture->height == 2);
        CHECK(texture && texture->rgba.size() == 16 && texture->rgba[0] == 255 && texture->rgba[4] == 0 && texture->rgba[5] == 255);
        const auto size = assets.textureSize("square_texture");
        CHECK(size && size->first == 2 && size->second == 2);
        CHECK(assets.byName(testres::kCres, "square_cres").has_value());
        CHECK(assets.byName(testres::kCres, "SQUARE_CRES").has_value());  // names are found in any case
        CHECK(!assets.byName(testres::kCres, "other_cres").has_value());
        CHECK(!assets.texture("missing").has_value());
        CHECK(!assets.warnings.empty());
    }
    std::filesystem::remove_all(root);
}

// A character file names its own models with the group of the game session that saved it (0x6F...).
// Those names find the file's own resources while it is in use, and nothing once it is released or
// for a Sim kept in an object group.
void characterFilesFindTheirSessionGroupNames() {
    const auto root = testres::gameFolder("neots2-character-group-test", {});
    auto own = testres::squareModel("face");
    for (auto& r : own) {
        r.key.group = 0xFFFFFFFF;
    }
    const auto file = root / "N001_User00001.package";
    testres::writeFile(file, testres::package(own));
    {
        ts2::scene::Assets assets(root);
        const std::string name = "#0x6f000fbf!face_cres";
        CHECK(!assets.byName(testres::kCres, name).has_value());
        {
            const ts2::scene::Assets::CharacterInUse inUse(assets, file);
            CHECK(assets.byName(testres::kCres, name).has_value());
            CHECK(!assets.byName(testres::kCres, "#0x1c0532fa!face_cres").has_value());  // other groups as written
        }
        CHECK(!assets.byName(testres::kCres, name).has_value());
        {
            const ts2::scene::Assets::CharacterInUse inUse(assets, 0x7F000001u);
            CHECK(!assets.byName(testres::kCres, name).has_value());
        }
    }
    std::filesystem::remove_all(root);
}

// A PlantSim's plant skin is laid between its bare skin and its clothes, named as the game names
// the composite. Other Sims' clothes go straight over the skin.
void plantSkinLiesBetweenSkinAndClothes() {
    std::vector<testres::Resource> resources;
    const std::vector<std::uint8_t> pixel{255, 255, 255, 255};
    for (const std::string name : {"skin", "plant", "cloth"}) {
        resources.push_back({testres::keyOf(testres::kTxmt, name + "_txmt"),
                             testres::material(name + "_txmt", "StandardMaterial",
                                               {{"stdMatBaseTextureName", name + "_tex"}, {"stdMatBaseTextureEnabled", "true"}})});
        resources.push_back({testres::keyOf(testres::kTxtr, name + "_tex_txtr"), testres::texture(name + "_tex_txtr", 1, 1, pixel)});
    }
    const auto root = testres::gameFolder("neots2-plant-skin-test", resources);
    {
        ts2::scene::Assets assets(root);
        ts2::scene::SceneBuilder builder(assets);
        ts2::scene::SimLook look;
        look.skinBody = "skin";
        CHECK(ts2::scene::clothesOverSkin(builder, assets, look, "cloth") == "_skin_cloth");
        look.plantBody = "plant";
        CHECK(ts2::scene::clothesOverSkin(builder, assets, look, "cloth") == "_skin_plant_cloth");
        look.skinHair = "skin";
        look.plantHead = "plant";
        CHECK(ts2::scene::hairOverScalp(builder, assets, look, "cloth") == "_skin_plant_cloth");
    }
    std::filesystem::remove_all(root);
}

// A Sim wears the last of its outfit list's hairs, tops, bottoms and bodies, and every face and
// accessory.
void simsWearTheLastHairTopBottomAndBody() {
    using namespace ts2::scene;
    CHECK(wearsLastEntryOnly(kHairPart) && wearsLastEntryOnly(kTopPart) && wearsLastEntryOnly(kBodyPart) &&
          wearsLastEntryOnly(kBottomPart));
    CHECK(!wearsLastEntryOnly(kFacePart) && !wearsLastEntryOnly(0x20));
}

// The skin catalog lists parts per part bit in key order and keeps only those that match.
void catalogQueriesMatchAsTheGames() {
    using namespace ts2::scene;
    const auto part = [](std::uint32_t instance, std::uint32_t high, std::uint32_t parts, std::uint32_t category, const char* skin) {
        CatalogPart p;
        p.key = {0xEBCF3E27, 0x2C17B74A, instance, high};
        p.age = 0x08;
        p.gender = 1;
        p.parts = parts;
        p.category = category;
        p.skinTone = catalogGuid(skin);
        p.hairTone = catalogGuid("");
        return p;
    };
    OutfitCatalog catalog;
    // Sorted as the game sorts keys: the high instance before the instance.
    catalog.parts = {part(2, 0, 4, 0, ""), part(9, 0, 4 | 8, 1, "00000002-0000-0000-0000-000000000000"),
                     part(1, 1, 4, 8, ""), part(3, 1, 8, 0, "{00000003-0000-0000-0000-000000000000}")};
    const auto keys = [](const std::vector<const CatalogPart*>& found) {
        std::vector<std::uint32_t> out;
        for (const auto* p : found) {
            out.push_back(p->key.instance);
        }
        return out;
    };
    PartQuery q;
    q.age = 0x08;
    q.gender = 1;
    q.species = 1;
    q.part = 4;
    CHECK((keys(findParts(catalog, q)) == std::vector<std::uint32_t>{2, 9, 1}));
    q.category = 1;
    CHECK((keys(findParts(catalog, q)) == std::vector<std::uint32_t>{2, 9}));
    q.part = 4 | 8;
    q.category = 0;
    q.skinTone = "00000003-0000-0000-0000-000000000000";
    CHECK((keys(findParts(catalog, q)) == std::vector<std::uint32_t>{2, 1, 3}));
    q.skinTone.reset();
    CHECK((keys(findParts(catalog, q)) == std::vector<std::uint32_t>{2, 9, 1, 9, 3}));
    q.gender = 2;
    CHECK(findParts(catalog, q).empty());
}

// Old everyday hairs and one-gender faces are worn with outerwear too, as the game reads them.
void oldEverydayHairsAndFacesFitOuterwear() {
    using ts2::scene::partCategories;
    CHECK(partCategories({{"category", "1"}, {"outfit", "1"}}) == 0x1001);
    CHECK(partCategories({{"category", "1"}, {"outfit", "1"}, {"version", "5"}}) == 0x1);
    CHECK(partCategories({{"category", "3"}, {"outfit", "2"}, {"gender", "1"}, {"version", "5"}}) == 0x1003);
    CHECK(partCategories({{"category", "1"}, {"outfit", "2"}, {"gender", "3"}}) == 0x1);
    CHECK(partCategories({{"category", "8"}, {"outfit", "1"}}) == 0x8);
    CHECK(partCategories({{"category", "1"}, {"outfit", "4"}}) == 0x1);
}

// A teen ages as the game ages one: its hair becomes the adult hair of its family, its own adult
// face is put on, the bare skin outfit and new clothes come from the catalog, and an outfit the
// new age has nothing for keeps the old lists.
void teensAgeIntoAdults() {
    using namespace ts2::scene;
    namespace sg = ts2::scenegraph;
    const auto key = [](std::uint32_t instance) { return sg::Link{0xEBCF3E27, 0x2C17B74A, instance, 0}; };
    const auto part = [&](std::uint32_t instance, std::map<std::string, std::string> p) { return catalogPart(key(instance), std::move(p)); };
    OutfitCatalog catalog;
    catalog.parts = {part(1, {{"age", "8"}, {"gender", "2"}, {"outfit", "1"}, {"category", "1"}, {"family", "aaaa"}, {"hairtone", "3"}}),
                     part(2, {{"age", "4"}, {"gender", "2"}, {"outfit", "1"}, {"category", "1"}, {"family", "aaaa"}, {"hairtone", "3"}}),
                     part(3, {{"age", "8"}, {"gender", "2"}, {"outfit", "4"}, {"category", "128"}}),
                     part(4, {{"age", "8"}, {"gender", "2"}, {"outfit", "8"}, {"category", "1"}, {"flags", "4"}}),
                     part(5, {{"age", "16"}, {"gender", "2"}, {"outfit", "1"}, {"category", "1"}, {"family", "aaaa"}, {"hairtone", "5"}})};
    catalog.skinTones = {catalogPart({0x4C158081, 1, 1, 0}, {{"age", "127"}, {"gender", "3"}, {"family", "bbbb"}})};
    const std::vector<CatalogPart> own = {catalogPart({0xEBCF3E27, 0xFFFFFFFF, 2, 0}, {{"age", "8"}, {"gender", "2"}, {"outfit", "2"}})};
    AgeData teen;
    teen.properties = {{"age", "4"}, {"gender", "2"}, {"species", "1"}, {"skincolor", "bbbb"}, {"haircolor", "3"}, {"stretch", "0.94"}};
    teen.lists[0x10001] = {key(2)};
    teen.lists[0x10002] = {{0xEBCF3E27, 0xFFFFFFFF, 1, 0}};
    teen.lists[0x4000001] = {key(2)};
    teen.lists[0x4000008] = {key(9)};
    OutfitRandom random(1);
    const auto adult = ageUp(teen, 8, own, catalog, random);
    CHECK(adult.has_value());
    if (!adult) {
        return;
    }
    CHECK(adult->properties.at("age") == "8" && adult->properties.at("stretch") == "1");
    CHECK(adult->lists.at(0x10001).front().instance == 1);
    CHECK(adult->lists.at(0x10002).front().instance == 2 && adult->lists.at(0x10002).front().group == 0xFFFFFFFF);
    CHECK(adult->lists.at(0x800004).front().instance == 3);
    CHECK(adult->lists.at(0x10008).front().instance == 4 && !adult->lists.contains(0x10004));
    // Outerwear-less hair and an outfit left out of the fill keep the teen's lists.
    CHECK(adult->lists.at(0x4000001).front().instance == 2 && adult->lists.at(0x4000008).front().instance == 9);
    const auto elder = ageUp(*adult, 0x10, own, catalog, random);
    CHECK(elder && elder->properties.at("haircolor") == "00000005-0000-0000-0000-000000000000" &&
          elder->lists.at(0x10001).front().instance == 5);
}

// A bus driver made by Make New Character: a skin of its own, hair and eyebrows of the drawn hair
// colour, eyes, a face template and the bare skin outfit from the catalog, and the uniform's body
// in place of the drawn everyday clothes. Glasses of flag 8 are never drawn.
void madeSimsWearTheirUniform() {
    using namespace ts2::scene;
    const auto key = [](std::uint32_t type, std::uint32_t instance) { return sg::Link{type, 0x2C17B74A, instance, 0}; };
    const auto gzps = [&](std::uint32_t instance, std::map<std::string, std::string> p) {
        return catalogPart(key(0xEBCF3E27, instance), std::move(p));
    };
    OutfitCatalog catalog;
    catalog.parts = {gzps(1, {{"age", "8"}, {"gender", "2"}, {"outfit", "1"}, {"category", "1"}, {"hairtone", "cccc"}}),
                     gzps(3, {{"age", "8"}, {"gender", "2"}, {"outfit", "2"}, {"category", "128"}}),
                     gzps(4, {{"age", "8"}, {"gender", "2"}, {"outfit", "8"}, {"category", "1"}})};
    catalog.skinTones = {catalogPart(key(0x4C158081, 1), {{"age", "127"}, {"gender", "3"}, {"family", "bbbb"}})};
    catalog.hairTones = {catalogPart(key(0x8C1580B5, 1), {{"age", "127"}, {"gender", "3"}, {"family", "cccc"}})};
    catalog.faceOverlays = {catalogPart(key(0x2C1FD8A1, 1), {{"age", "8"}, {"gender", "3"}, {"subtype", "1"}, {"hairtone", "cccc"}}),
                            catalogPart(key(0x2C1FD8A1, 2), {{"age", "8"}, {"gender", "3"}, {"subtype", "3"}, {"family", "eeee"}}),
                            catalogPart(key(0x2C1FD8A1, 3), {{"age", "8"}, {"gender", "3"}, {"subtype", "5"}, {"flags", "8"}})};
    catalog.faceTemplates = {catalogPart(key(0x8C93E35C, 1), {{"age", "8"}, {"gender", "2"}, {"family", "ffff"}})};
    AgeData uniform;
    uniform.properties = {{"age", "8"}, {"gender", "2"}, {"species", "1"}, {"validparts", "992"}};
    uniform.lists[0x10008] = {key(0xEBCF3E27, 9)};
    OutfitRandom random(1);
    const auto made = makeLook({8, 2, 2}, {uniform}, catalog, random);
    const auto& d = made.data;
    CHECK(d.properties.at("skincolor") == "00000002-0000-0000-0000-000000000000");
    CHECK(d.properties.at("haircolor") == "cccc" && d.properties.at("eyecolor") == "eeee");
    CHECK(d.lists.at(0x10001).front().instance == 1 && d.lists.at(0x800002).front().instance == 3);
    CHECK(d.lists.at(0x10008).size() == 1 && d.lists.at(0x10008).front().instance == 9);
    CHECK(made.faceFamily == "ffff" && made.problems.empty());
    CHECK(made.faceOverlays.size() == 2 && made.faceOverlays[0].instance == 1 && made.faceOverlays[1].instance == 2);
}

// Change Outfit picks among an outfit object's age data those of the Sim's age and gender (or of
// none), one drawn from the Sim's GUID when there are several, and puts on what its lock names.
void outfitsArePickedAndPutOn() {
    using namespace ts2::scene;
    const auto key = [](std::uint32_t instance) { return sg::Link{0xEBCF3E27, 0x2C17B74A, instance, 0}; };
    const auto outfit = [&](const char* age, const char* gender, std::uint32_t part) {
        AgeData d;
        d.properties = {{"age", age}, {"gender", gender}, {"validparts", "992"}};
        d.lists[0x10008] = {key(part)};
        return d;
    };
    const std::vector<AgeData> outfits = {outfit("8", "1", 1), outfit("2", "2", 2), outfit("8", "1", 3), outfit("0", "0", 4)};
    CHECK(pickOutfit(outfits, 4, 2, 7) == &outfits[3]);  // only the one of no age and gender fits a teen male
    const std::vector<AgeData> adultOnly = {outfits[0]};
    CHECK(pickOutfit(adultOnly, 4, 1, 7) == nullptr);
    // Three fit an adult female: the draw from the GUID picks the same one each time.
    const AgeData* first = pickOutfit(outfits, 8, 1, 0x51515151);
    CHECK(first != nullptr && first != &outfits[1] && pickOutfit(outfits, 8, 1, 0x51515151) == first);
    AgeData sim;
    sim.properties = {{"age", "8"}, {"gender", "1"}};
    sim.lists[0x10004] = {key(10)};
    sim.lists[0x10010] = {key(11)};
    sim.lists[0x80008] = {key(12)};
    CHECK(putOn(sim, outfits[0]).empty());
    CHECK(sim.lists.at(0x10008).front().instance == 1 && !sim.lists.contains(0x10004) && !sim.lists.contains(0x10010));
    CHECK(sim.lists.at(0x80008).front().instance == 12);
}

// A face template's vertices take the place of the base face's at their texture coordinates, while
// the hair and neck stay where the base has them.
void faceTemplatesMoveTheFace() {
    auto element = [](std::uint32_t identity, std::uint32_t format, const std::vector<float>& values) {
        sg::Element e;
        e.identity = identity;
        e.blockFormat = format;
        e.count = static_cast<std::uint32_t>(values.size() / (format + 1));
        for (float v : values) {
            const auto bits = std::bit_cast<std::uint32_t>(v);
            for (int i = 0; i < 4; ++i) {
                e.data.push_back(static_cast<std::uint8_t>(bits >> (8 * i)));
            }
        }
        return e;
    };
    auto mesh = [&](const std::vector<float>& positions, const std::vector<float>& uv) {
        sg::Mesh m;
        m.elements.push_back(element(sg::kPositions, 2, positions));
        m.elements.push_back(element(sg::kTextureCoordinates, 1, uv));
        sg::ElementLink link;
        link.elements = {0, 1};
        link.vertexCount = static_cast<std::uint32_t>(uv.size() / 2);
        m.links.push_back(link);
        sg::Group g;
        g.primitive = sg::kTriangleList;
        g.indices = {0, 1, 2};
        m.groups.push_back(g);
        return m;
    };
    auto base = mesh({0, 0, 0, 1, 0, 0, 0, 1, 0}, {0, 0, 1, 0, 0, 1});
    const auto from = mesh({5, 5, 5, 7, 7, 7}, {1, 0, 0, 1});
    ts2::scene::FaceMasks masks;
    masks["hairneck"] = {{0, 1, 1}};
    ts2::scene::applyFaceTemplate(base, from, masks);
    CHECK(sg::vertexFloats(base, base.links[0], sg::kPositions) == std::vector<float>({0, 0, 0, 5, 5, 5, 0, 1, 0}));
}

// A model's resource node leads to its shape, geometry node, mesh, material and texture.
void modelsAreBuiltFromTheirResources() {
    const auto root = testres::gameFolder("neots2-builder-test", testres::squareModel("square"));
    {
        ts2::scene::Assets assets(root);
        ts2::scene::SceneBuilder builder(assets);
        const int model = builder.model("square", {});
        CHECK(model >= 0);
        if (model >= 0) {
            const auto& scene = builder.scene();
            const auto& mesh = scene.meshes[static_cast<std::size_t>(model)];
            CHECK(mesh.primitives.size() == 1);
            if (mesh.primitives.size() == 1) {
                const auto& p = mesh.primitives[0];
                CHECK(p.group == "square" && p.positions.size() == 12 && p.indices.size() == 6 && p.uvs.size() == 8);
                CHECK(p.material >= 0);
                if (p.material >= 0) {
                    const auto& m = scene.materials[static_cast<std::size_t>(p.material)];
                    CHECK(m.image >= 0 && static_cast<std::size_t>(m.image) < scene.images.size());
                    CHECK(m.image >= 0 && scene.images[static_cast<std::size_t>(m.image)].width == 2);
                }
            }
        }
        CHECK(builder.model("square", {}) == model);  // built once, then shared
        CHECK(builder.model("nothing", {}) == -1 && builder.missingModels == 1);
    }
    std::filesystem::remove_all(root);
}

// A shape is drawn where its reference node sits in the model's tree.
void shapesSitWhereTheirNodePutsThem() {
    auto resources = testres::squareModel("square");
    resources[0].data = testres::placedResourceNode("square_cres", "square_shpe", {2, 0, 0.5f});
    const auto tree = ts2::scenegraph::parseResourceNode(resources[0].data);
    CHECK(tree.nodes.size() == 2 && tree.nodes[0].kind == ts2::scenegraph::SceneNode::Kind::Root);
    CHECK(tree.nodes.size() == 2 && tree.nodes[1].kind == ts2::scenegraph::SceneNode::Kind::ShapeRef &&
          tree.nodes[1].shapeLinks.size() == 1);
    const auto root = testres::gameFolder("neots2-builder-tree-test", resources);
    {
        ts2::scene::Assets assets(root);
        ts2::scene::SceneBuilder builder(assets);
        const int model = builder.model("square", {});
        CHECK(model >= 0);
        if (model >= 0) {
            const auto& p = builder.scene().meshes[static_cast<std::size_t>(model)].primitives.front();
            CHECK(p.positions.size() == 12 && p.positions[0] == 2 && p.positions[2] == 0.5f && p.positions[3] == 3);
            CHECK(p.origin[0] == 2 && p.origin[1] == 0 && p.origin[2] == 0.5f);
            CHECK(builder.bone("square", "shape") != nullptr);
        }
    }
    std::filesystem::remove_all(root);
}

// A lot saves an object's joints as it posed them (a dining chair's seat slid under a table): the
// model is drawn with them, and its bones are known under the posed name. Joints saved at rest
// share the model's own mesh.
void savedJointsMoveTheModel() {
    auto resources = testres::squareModel("square");
    resources[0].data = testres::jointedResourceNode("square_cres", "square_shpe", "Slot_Seat");
    const auto root = testres::gameFolder("neots2-builder-saved-pose-test", resources);
    {
        ts2::scene::Assets assets(root);
        ts2::scene::SceneBuilder builder(assets);
        const int rest = builder.model("square", {});
        std::string boneKey;
        CHECK(builder.savedPoseModel("square", {}, std::vector<ts2::cobject::SavedJoint>{{"slot_seat", {0, 0, 0}}}, boneKey) == rest);
        CHECK(boneKey == "square");
        const std::vector<ts2::cobject::SavedJoint> pushed{{"slot_seat", {0, 0.5f, 0}}, {"elsewhere", {9, 9, 9}}};
        const int posed = builder.savedPoseModel("square", {}, pushed, boneKey);
        CHECK(posed >= 0 && posed != rest && boneKey != "square");
        if (posed >= 0 && rest >= 0) {
            const auto& a = builder.scene().meshes[static_cast<std::size_t>(rest)].primitives.front().positions;
            const auto& b = builder.scene().meshes[static_cast<std::size_t>(posed)].primitives.front().positions;
            CHECK(a.size() == b.size() && b[1] == a[1] + 0.5f && b[0] == a[0] && b[2] == a[2]);
        }
        const auto* seat = builder.bone(boneKey, "slot_seat");
        const auto* restSeat = builder.bone("square", "slot_seat");
        CHECK(seat != nullptr && seat->m[7] == 0.5f && restSeat != nullptr && restSeat->m[7] == 0);
        std::string again;
        CHECK(builder.savedPoseModel("square", {}, pushed, again) == posed && again == boneKey);  // shared
    }
    std::filesystem::remove_all(root);
}

// A model whose material is missing is still built, without a material. One whose mesh is missing
// has nothing to draw.
void modelsWithMissingPartsAreHandled() {
    auto resources = testres::squareModel("square");
    std::erase_if(resources, [](const testres::Resource& r) { return r.key.type == testres::kTxmt; });
    auto broken = testres::squareModel("broken");
    std::erase_if(broken, [](const testres::Resource& r) { return r.key.type == testres::kGmdc; });
    resources.insert(resources.end(), broken.begin(), broken.end());
    const auto root = testres::gameFolder("neots2-builder-missing-test", resources);
    {
        ts2::scene::Assets assets(root);
        ts2::scene::SceneBuilder builder(assets);
        const int model = builder.model("square", {});
        CHECK(model >= 0);
        if (model >= 0) {
            const auto& mesh = builder.scene().meshes[static_cast<std::size_t>(model)];
            CHECK(mesh.primitives.size() == 1 && mesh.primitives[0].material < 0);
        }
        CHECK(builder.missingMaterials == 1);
        CHECK(builder.model("broken", {}) == -1);
    }
    std::filesystem::remove_all(root);
}

// A standard material reflects its environment cube only with a mode other than "none", a cube
// named and a coefficient adding to at least 0.003.
void envCubeReflectionTurnsOnAsTheGames() {
    sg::Material m;
    m.type = "StandardMaterial";
    m.properties = {{"stdMatEnvCubeMode", "reflection"},
                    {"stdMatEnvCubeTextureName", "reflectionkitchenhighcontrast-envcube"},
                    {"stdMatEnvCubeCoef", "0.1,0.2,0.3"},
                    {"stdMatEnvCubeLockedToCamera", "1"}};
    const auto on = ts2::scene::envCubeSettings(m);
    CHECK(on && on->texture == "reflectionkitchenhighcontrast-envcube" && on->lockedToCamera);
    CHECK(on && on->coef[0] == 0.1f && on->coef[1] == 0.2f && on->coef[2] == 0.3f);
    m.properties["stdMatEnvCubeLockedToCamera"] = "0";
    CHECK(ts2::scene::envCubeSettings(m) && !ts2::scene::envCubeSettings(m)->lockedToCamera);
    m.properties["stdMatEnvCubeCoef"] = "0.001,0.001,0.0009";
    CHECK(!ts2::scene::envCubeSettings(m));
    m.properties["stdMatEnvCubeCoef"] = "0.001,0.001,0.0011";
    CHECK(ts2::scene::envCubeSettings(m).has_value());
    m.properties["stdMatEnvCubeCoef"] = "inf,0,0";
    CHECK(!ts2::scene::envCubeSettings(m));
    m.properties["stdMatEnvCubeCoef"] = "1,1,1";
    m.properties["stdMatEnvCubeMode"] = "none";
    CHECK(!ts2::scene::envCubeSettings(m));
    m.properties["stdMatEnvCubeMode"] = "reflection";
    m.properties.erase("stdMatEnvCubeTextureName");
    CHECK(!ts2::scene::envCubeSettings(m));
}

// Specular only with a power above 1 and a scale adding to more than 0.3, at 0.4 times the power.
void specularTurnsOnAsTheGames() {
    sg::Material m;
    m.type = "StandardMaterial";
    m.properties = {{"stdMatSpecPower", "10"}, {"stdMatSpecCoef", "0.2,0.2,0.2"}};
    const auto on = ts2::scene::specularSettings(m);
    CHECK(on && on->power == 4.0f && on->coef[0] == 0.2f);
    m.properties["stdMatSpecPower"] = "1";
    CHECK(!ts2::scene::specularSettings(m));
    m.properties["stdMatSpecPower"] = "10";
    m.properties["stdMatSpecCoef"] = "0.1,0.1,0.05";
    CHECK(!ts2::scene::specularSettings(m));
    m.properties["stdMatSpecCoef"] = "nan,1,1";
    CHECK(!ts2::scene::specularSettings(m));
}

// The ground around a lot meets the lot's own ground along the lot's edges, also on a lot placed
// turned, and keeps the neighborhood's heights elsewhere.
void lotSkirtMeetsTheLotsEdges() {
    using namespace ts2::scene;
    PaintedTerrain terrain;
    terrain.columns = terrain.rows = 6;
    terrain.heights.assign(36, 5.0f);
    LotFootprint lot;
    lot.x0 = lot.y0 = 20;
    lot.x1 = lot.y1 = 30;
    lot.z = 5;
    // A 10 x 10 tile lot a quarter turn round, its ground falling 0.1 a tile along its own x.
    lot.place = Transform::at(20, 30, 5, -1.5707963267948966f);
    auto grid = std::make_shared<ts2::array3d::Array3D>();
    grid->width = grid->height = 11;
    grid->levels = 1;
    grid->elementSize = 4;
    for (std::uint32_t x = 0; x < 11; ++x) {
        for (std::uint32_t y = 0; y < 11; ++y) {
            const float h = -0.1f * static_cast<float>(x);
            const auto* bytes = reinterpret_cast<const std::uint8_t*>(&h);
            grid->elements.insert(grid->elements.end(), bytes, bytes + 4);
        }
    }
    // The elements go level, x, then y, so the loop above filled them in order.
    const Heights heights(grid);
    const auto root = testres::gameFolder("neots2-skirt-test", {});
    {
        Assets assets(root);
        SceneBuilder builder(assets);
        addLotSkirt(builder, terrain, lot, 2, "Temperate", {}, &heights, 0);
        auto heightAt = [&](float x, float y) {
            for (const auto& mesh : builder.scene().meshes) {
                for (const auto& p : mesh.primitives) {
                    for (std::size_t i = 0; i + 2 < p.positions.size(); i += 3) {
                        if (p.positions[i] == x && p.positions[i + 1] == y) {
                            return p.positions[i + 2];
                        }
                    }
                }
            }
            return -100.0f;
        };
        // The lot's own (0, 0) lies at world (20, 30) and its own x runs toward world -y.
        CHECK(std::abs(heightAt(20, 30) - 5.0f) < 1e-4f);
        CHECK(std::abs(heightAt(20, 20) - 4.0f) < 1e-4f);
        CHECK(std::abs(heightAt(30, 20) - 4.0f) < 1e-4f);
        CHECK(std::abs(heightAt(30, 30) - 5.0f) < 1e-4f);
        CHECK(heightAt(10, 20) == 5.0f && heightAt(40, 40) == 5.0f);
    }
    std::filesystem::remove_all(root);
}

} // namespace

std::vector<Test> assetTests() {
    return {
        {"hand-made resources read back", handMadeResourcesReadBack},
        {"assets find resources by name", assetsFindResourcesByName},
        {"character files find their session group names", characterFilesFindTheirSessionGroupNames},
        {"plant skin lies between skin and clothes", plantSkinLiesBetweenSkinAndClothes},
        {"sims wear the last hair, top, bottom and body", simsWearTheLastHairTopBottomAndBody},
        {"catalog queries match as the game's", catalogQueriesMatchAsTheGames},
        {"old everyday hairs and faces fit outerwear", oldEverydayHairsAndFacesFitOuterwear},
        {"teens age into adults", teensAgeIntoAdults},
        {"made Sims wear their uniform", madeSimsWearTheirUniform},
        {"outfits are picked and put on", outfitsArePickedAndPutOn},
        {"face templates move the face", faceTemplatesMoveTheFace},
        {"models are built from their resources", modelsAreBuiltFromTheirResources},
        {"models with missing parts are handled", modelsWithMissingPartsAreHandled},
        {"saved joints move the model", savedJointsMoveTheModel},
        {"shapes sit where their node puts them", shapesSitWhereTheirNodePutsThem},
        {"env cube reflection turns on as the game's", envCubeReflectionTurnsOnAsTheGames},
        {"specular turns on as the game's", specularTurnsOnAsTheGames},
        {"lot skirt meets the lot's edges", lotSkirtMeetsTheLotsEdges},
    };
}
