#include "sim/world.hpp"

#include <stdexcept>
#include <string>

namespace ts2::sim {

SimObject& World::create(std::uint32_t group, objd::ObjectDefinition definition, std::size_t containerSlots) {
    if (nextId_ <= 0) {
        throw std::runtime_error("World: out of object ids");
    }
    auto object = std::make_unique<SimObject>();
    object->id = nextId_++;
    object->group = group;
    object->definition = std::move(definition);
    object->data[kObjectIdField] = object->id;
    object->arrays.resize(object->definition.field(kObjectArrayCountField));
    object->arrayIterators.resize(object->arrays.size(), 0);
    object->slots.resize(containerSlots, 0);

    SimObject& ref = *object;
    objects_.emplace(ref.id, std::move(object));
    return ref;
}

SimObject& World::restore(std::int16_t id, std::uint32_t group, objd::ObjectDefinition definition) {
    if (id <= 0 || objects_.contains(id)) {
        throw std::runtime_error("World: object id " + std::to_string(id) + " is not free");
    }
    auto object = std::make_unique<SimObject>();
    object->id = id;
    object->group = group;
    object->definition = std::move(definition);
    object->arrays.resize(object->definition.field(kObjectArrayCountField));
    object->arrayIterators.resize(object->arrays.size(), 0);

    SimObject& ref = *object;
    objects_.emplace(id, std::move(object));
    if (id >= nextId_) {
        nextId_ = static_cast<std::int16_t>(id + 1);
    }
    return ref;
}

SimObject* World::find(std::int16_t id) {
    const auto it = objects_.find(id);
    return it != objects_.end() && !it->second->removed ? it->second.get() : nullptr;
}

void World::remove(SimObject& object) {
    if (!object.removed) {
        object.removed = true;
        ++removedCount_;
    }
}

void World::purge() {
    std::erase_if(objects_, [](const auto& entry) { return entry.second->removed; });
    std::erase_if(relationships_, [this](const auto& entry) {
        return !objects_.contains(entry.first.first) || !objects_.contains(entry.first.second);
    });
    removedCount_ = 0;
}

} // namespace ts2::sim
