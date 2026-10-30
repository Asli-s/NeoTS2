#pragma once

#include "content/resource_index.hpp"
#include "sim/code_source.hpp"

#include <map>
#include <optional>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace ts2::sim {

class IndexCodeSource : public CodeSource {
public:
    explicit IndexCodeSource(content::ResourceIndex& index);

    std::shared_ptr<const bhav::Behavior> behavior(std::uint32_t group,
                                                   std::uint16_t instance) override;
    const std::vector<std::int16_t>* constants(std::uint32_t group,
                                               std::uint16_t instance) override;
    std::uint32_t semiGlobalGroup(std::uint32_t objectGroup) override;
    const ObjectType* objectType(std::uint32_t guid) override;
    const ObjectType* multiTileMaster(const ObjectType& part) override;
    const ObjectType* addMadeObjectType(ObjectType type) override;
    const std::vector<std::uint32_t>& objectTypeGuids() override;
    const std::map<std::string, std::string>* ageData(std::uint32_t group) override;
    std::optional<std::vector<std::uint8_t>> characterResource(std::uint32_t type, std::uint32_t group) override;
    [[nodiscard]] std::optional<std::string> string(std::uint32_t group, std::uint16_t instance,
                                      std::size_t index) override;
    [[nodiscard]] std::optional<std::string> stringDescription(std::uint32_t group, std::uint16_t instance, std::size_t index) override;
    [[nodiscard]] std::optional<std::string> catalogString(std::uint32_t group, std::uint16_t instance, std::size_t index) override;
    [[nodiscard]] std::optional<tga::Pixels> image(std::uint32_t group, std::uint32_t instance, std::uint32_t maxSide) override;
    [[nodiscard]] std::optional<std::string> interactionString(std::uint32_t group, std::uint16_t instance,
                                                 std::size_t index) override;
    [[nodiscard]] std::optional<std::uint16_t> behaviorByName(std::uint32_t group, const std::string& name) override;
    std::shared_ptr<const ttab::InteractionTable> interactionTable(std::uint32_t group,
                                                                  std::uint16_t instance) override;
    const std::vector<objf::Function>* objectFunctions(const ObjectType& type) override;
    const fcns::Tunings* tunings() override;
    const std::vector<slot::Entry>& globalSlots() override;

    const std::unordered_map<std::uint32_t, std::uint32_t>& semiGlobalGroups() const {
        return semiGlobals_;
    }

private:
    content::ResourceIndex& index_;
    std::unordered_map<std::uint64_t, std::shared_ptr<const bhav::Behavior>> behaviors_;
    std::unordered_map<std::uint64_t, std::shared_ptr<const ttab::InteractionTable>> interactionTables_;
    std::unordered_map<std::uint64_t, std::optional<std::vector<std::int16_t>>> constants_;
    std::unordered_map<std::uint32_t, std::uint32_t> semiGlobals_;
    std::unordered_map<std::uint32_t, ObjectType> objectTypes_;
    std::vector<std::uint32_t> objectTypeGuids_;
    // Multi-tile masters by (group << 16 | multi-tile id): their GUIDs.
    std::unordered_map<std::uint64_t, std::uint32_t> multiTileMasters_;
    std::unordered_map<std::uint64_t, std::optional<std::vector<objf::Function>>> objectFunctions_;
    bool objectTypesLoaded_ = false;
    std::unordered_set<std::uint32_t> groupsWithTrees_;
    std::unordered_set<std::uint32_t> simGroups_;  // groups of Sims' own types
    // The group a type's own trees, constants and object functions are read from: its own, or for a
    // Sim whose character file carries no trees, the template person's, as the game does.
    std::uint32_t privateCodeGroup(std::uint32_t group);
    const std::vector<std::string>& strings(std::uint32_t type, std::uint32_t group, std::uint16_t instance,
                                           bool descriptions = false);
    // Each table's US English texts, by (type, group, instance).
    std::map<std::tuple<std::uint32_t, std::uint32_t, std::uint16_t>, std::vector<std::string>> strings_;
    std::map<std::tuple<std::uint32_t, std::uint32_t, std::uint16_t>, std::vector<std::string>> descriptions_;
    std::map<std::uint32_t, std::optional<std::map<std::string, std::string>>> ageData_;
    std::unordered_map<std::uint32_t, std::unordered_map<std::string, std::uint16_t>> behaviorNames_;
    bool behaviorNamesLoaded_ = false;
    std::optional<std::optional<fcns::Tunings>> tunings_;
    std::optional<std::vector<slot::Entry>> globalSlots_;
};

} // namespace ts2::sim
