#include "sim/type_signature.hpp"

#include <memory>
#include <vector>

namespace ts2::sim {

namespace {

// Tree ids: globals below 0x1000, the object's own trees below 0x2000, its semi-globals above.
// Opcodes from 0x100 to 0x7FFF call trees. Ids above 0x7FFF are never calls.
constexpr std::uint16_t kFirstTreeId = 0x0100;
constexpr std::uint16_t kLastTreeId = 0x7FFF;

std::shared_ptr<const bhav::Behavior> findTree(CodeSource& code, std::uint32_t objectGroup, std::uint16_t id) {
    std::uint32_t group = kGlobalGroup;
    if (id >= 0x2000) {
        group = code.semiGlobalGroup(objectGroup);
    } else if (id >= 0x1000) {
        group = objectGroup;
    }
    return code.behavior(group, id);
}

} // namespace

std::uint32_t treeSignature(CodeSource& code, std::uint32_t objectGroup, std::uint16_t tree) {
    if (tree == 0) {
        return 0;
    }
    // A walk over the call graph with an explicit stack: every tree id is visited at most once, so
    // the work and memory are bounded however the trees call each other.
    std::vector<bool> visited(kLastTreeId + 1, false);
    std::vector<std::uint16_t> pending{tree};
    std::uint32_t versions = 0;
    while (!pending.empty()) {
        const std::uint16_t id = pending.back();
        pending.pop_back();
        if (id > kLastTreeId || visited[id]) {
            continue;
        }
        visited[id] = true;
        const auto behavior = findTree(code, objectGroup, id);
        if (behavior == nullptr) {
            continue;
        }
        versions += behavior->treeVersion;
        for (const auto& instruction : behavior->instructions) {
            if (instruction.opcode >= kFirstTreeId && instruction.opcode <= kLastTreeId && !visited[instruction.opcode]) {
                pending.push_back(instruction.opcode);
            }
        }
    }
    const auto start = findTree(code, objectGroup, tree);
    const std::uint32_t startVersion = start ? start->treeVersion : 0;
    return (startVersion << 16) + versions;
}

bool typeChangedSinceSave(CodeSource& code, const ObjectType& type, const objt::Entry& saved) {
    // The saved values, as five 32-bit words: Init and Main signatures, OBJD 0x3A, OBJD 0x3B, and one
    // the game doesn't compare.
    auto word = [&](std::size_t i) {
        return static_cast<std::uint32_t>(saved.unknown[2 * i]) | static_cast<std::uint32_t>(saved.unknown[2 * i + 1]) << 16;
    };
    const auto* functions = code.objectFunctions(type);
    auto signature = [&](std::size_t entry) -> std::uint32_t {
        if (functions == nullptr || entry >= functions->size()) {
            return 0;
        }
        return treeSignature(code, type.group, (*functions)[entry].action);
    };
    return signature(0) != word(0) || signature(1) != word(1) || type.definition.field(0x3A) != word(2) ||
           type.definition.field(0x3B) != word(3);
}

} // namespace ts2::sim
