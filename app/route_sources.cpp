#include "route_sources.hpp"

#include "content/lot.hpp"
#include "diag/log.hpp"
#include "formats/scenegraph.hpp"
#include "scene/scene.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <map>
#include <memory>

namespace app {

namespace {
namespace sg = ts2::scenegraph;

constexpr std::uint32_t kResourceNodeType = 0xE519C933;
constexpr std::uint32_t kShapeType = 0xFC6EB1F7;
constexpr std::uint32_t kGeometryNodeType = 0x7BA3838C;
constexpr std::uint32_t kMeshType = 0xAC4F8687;
} // namespace

std::optional<ts2::sim::RouteRect> modelExtent(ts2::scene::Assets& assets, const std::string& name) {
    const auto cres = assets.byName(kResourceNodeType, name + "_cres");
    if (!cres) {
        return std::nullopt;
    }
    constexpr float kMax = std::numeric_limits<float>::max();
    float lo[2] = {kMax, kMax}, hi[2] = {-kMax, -kMax};
    for (const auto& link : sg::parseResourceNode(*cres).container.links) {
        if (link.type != kShapeType) {
            continue;
        }
        const auto data = assets.byKey(link);
        if (!data) {
            continue;
        }
        const auto shape = sg::parseShape(*data);
        for (std::size_t i = 0; i < shape.geometryNodes.size() && i < shape.detailLevels.size(); ++i) {
            if (shape.detailLevels[i] != 0) {
                continue;
            }
            const auto gmnd = assets.byName(kGeometryNodeType, shape.geometryNodes[i]);
            if (!gmnd) {
                continue;
            }
            for (const auto& l : sg::parseContainer(*gmnd).links) {
                if (l.type != kMeshType) {
                    continue;
                }
                const auto bytes = assets.byKey(l);
                if (!bytes) {
                    continue;
                }
                const auto mesh = sg::parseMesh(*bytes);
                for (const auto& g : mesh.groups) {
                    if (g.link >= mesh.links.size()) {
                        continue;
                    }
                    const auto pos = sg::vertexFloats(mesh, mesh.links[g.link], sg::kPositions);
                    for (std::size_t v = 0; v + 2 < pos.size(); v += 3) {
                        for (int k = 0; k < 2; ++k) {
                            lo[k] = std::min(lo[k], pos[v + k]);
                            hi[k] = std::max(hi[k], pos[v + k]);
                        }
                    }
                }
            }
        }
    }
    if (lo[0] > hi[0]) {
        return std::nullopt;
    }
    return ts2::sim::RouteRect{lo[0], lo[1], hi[0], hi[1]};
}

namespace {

std::string lower(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

// Every named node of a model's tree at rest, placed in the model (its parents' transforms applied),
// and the tree itself in the order it is walked.
void modelBones(const sg::ResourceNode& tree, ts2::sim::RouteModel& model) {
    constexpr int kMaxDepth = 64;  // a broken tree could nest without end
    auto& bones = model.bones;
    std::vector<bool> seen(tree.nodes.size(), false);
    std::vector<bool> isChild(tree.nodes.size(), false);
    for (const auto& n : tree.nodes) {
        for (const auto c : n.children) {
            if (c < isChild.size()) {
                isChild[c] = true;
            }
        }
    }
    auto walk = [&](auto&& self, std::uint32_t index, const ts2::scene::Transform& parent, int depth, int parentNode) -> void {
        if (index >= tree.nodes.size() || seen[index] || depth > kMaxDepth) {
            if (depth > kMaxDepth) {
                ts2::diag::warnOnce("route sources: bones nested deeper than 64 levels are dropped");
            }
            return;
        }
        seen[index] = true;
        const auto& node = tree.nodes[index];
        const ts2::scene::Transform place = parent * ts2::scene::Transform::of(node.translation, node.rotation);
        if (!node.name.empty()) {
            float origin[3] = {0, 0, 0};
            place.apply(origin, true);
            bones.try_emplace(lower(node.name), ts2::sim::ModelBone{origin[0], origin[1], std::atan2(place.m[4], place.m[0])});
        }
        const int here = static_cast<int>(model.nodes.size());
        model.nodes.push_back({node.name.empty() ? std::string() : lower(node.name), parentNode, node.translation, node.rotation});
        for (const auto c : node.children) {
            self(self, c, place, depth + 1, here);
        }
    };
    for (std::uint32_t i = 0; i < tree.nodes.size(); ++i) {
        if (!isChild[i]) {
            walk(walk, i, ts2::scene::Transform{}, 0, -1);
        }
    }
}

ts2::sim::RouteModel readRouteModel(ts2::scene::Assets& assets, const std::string& name) {
    ts2::sim::RouteModel model;
    if (const auto cres = assets.byName(kResourceNodeType, name + "_cres")) {
        try {
            for (auto& list : sg::parseDataLists(*cres)) {
                if (list.name == "footprint") {
                    model.footprint = std::move(list);
                }
            }
            model.extent = modelExtent(assets, name);
            modelBones(sg::parseResourceNode(*cres), model);
        } catch (const std::exception&) {
            // A broken model routes as if it had no footprint data.
        }
    }
    return model;
}

std::optional<ts2::sim::RouteRect> readPostFootprint(ts2::scene::Assets& assets, std::uint32_t fenceGuid) {
    const auto* fence = assets.fence(fenceGuid);
    if (fence == nullptr || fence->mayWalkOver) {
        return std::nullopt;
    }
    try {
        return modelExtent(assets, fence->post);
    } catch (const std::exception&) {
        return std::nullopt;  // a broken post model adds no post
    }
}

} // namespace

std::shared_ptr<const RouteModelTable> readRouteModels(ts2::scene::Assets& assets, const std::filesystem::path& lot) {
    auto table = std::make_shared<RouteModelTable>();
    const ts2::content::Lot saved = ts2::content::loadLot(lot);
    for (const auto& [object, name] : saved.objectModels) {
        if (!table->models.contains(name)) {
            table->models.emplace(name, readRouteModel(assets, name));
        }
    }
    for (const auto& post : saved.fencePosts) {
        if (!table->posts.contains(post.fence)) {
            table->posts.emplace(post.fence, readPostFootprint(assets, post.fence));
        }
    }
    return table;
}

ts2::sim::RouteSources routeSources(ts2::scene::Assets& assets, std::shared_ptr<const RouteModelTable> table) {
    if (!table) {
        table = std::make_shared<const RouteModelTable>();
    }
    ts2::sim::RouteSources sources;
    sources.fenceBlocks = [&assets](std::uint32_t guid) {
        const auto* fence = assets.fence(guid);
        return fence != nullptr && !fence->mayWalkOver;
    };
    auto posts = std::make_shared<std::map<std::uint32_t, std::optional<ts2::sim::RouteRect>>>();
    sources.postFootprint = [&assets, table, posts](std::uint32_t guid) {
        if (const auto it = table->posts.find(guid); it != table->posts.end()) {
            return it->second;
        }
        auto it = posts->find(guid);
        if (it == posts->end()) {
            it = posts->emplace(guid, readPostFootprint(assets, guid)).first;
        }
        return it->second;
    };
    // Map entries stay where they are, so what the lookup returns stays valid.
    auto models = std::make_shared<std::map<std::string, ts2::sim::RouteModel>>();
    sources.models = [&assets, table, models](const std::string& name) -> const ts2::sim::RouteModel* {
        if (const auto it = table->models.find(name); it != table->models.end()) {
            return &it->second;
        }
        auto it = models->find(name);
        if (it == models->end()) {
            it = models->emplace(name, readRouteModel(assets, name)).first;
        }
        return &it->second;
    };
    return sources;
}

} // namespace app
