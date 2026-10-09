#pragma once

#include "content/resource_index.hpp"
#include "sim/code_source.hpp"

#include <optional>
#include <unordered_map>

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
    std::optional<std::string> string(std::uint32_t group, std::uint16_t instance,
                                      std::size_t index) override;
    std::optional<std::uint16_t> behaviorByName(std::uint32_t group, const std::string& name) override;
    std::shared_ptr<const ttab::InteractionTable> interactionTable(std::uint32_t group,
                                                                  std::uint16_t instance) override;

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
    bool objectTypesLoaded_ = false;
    std::unordered_map<std::uint64_t, std::vector<std::string>> strings_;
    std::unordered_map<std::uint32_t, std::unordered_map<std::string, std::uint16_t>> behaviorNames_;
    bool behaviorNamesLoaded_ = false;
};

} // namespace ts2::sim
