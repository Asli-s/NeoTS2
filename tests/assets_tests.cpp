// Asset loading and model building, from hand-made packages (see test_resources.hpp).
#include "test_support.hpp"
#include "test_resources.hpp"

#include "formats/dbpf.hpp"
#include "scene/assets.hpp"
#include "scene/builder.hpp"
#include "scene/sims.hpp"

#include <filesystem>

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

} // namespace

std::vector<Test> assetTests() {
    return {
        {"hand-made resources read back", handMadeResourcesReadBack},
        {"assets find resources by name", assetsFindResourcesByName},
        {"character files find their session group names", characterFilesFindTheirSessionGroupNames},
        {"plant skin lies between skin and clothes", plantSkinLiesBetweenSkinAndClothes},
        {"sims wear the last hair, top, bottom and body", simsWearTheLastHairTopBottomAndBody},
        {"models are built from their resources", modelsAreBuiltFromTheirResources},
        {"models with missing parts are handled", modelsWithMissingPartsAreHandled},
        {"shapes sit where their node puts them", shapesSitWhereTheirNodePutsThem},
        {"env cube reflection turns on as the game's", envCubeReflectionTurnsOnAsTheGames},
        {"specular turns on as the game's", specularTurnsOnAsTheGames},
    };
}
