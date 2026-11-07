#include "scene/object_effects.hpp"

#include <algorithm>

namespace ts2::scene {

ObjectEffects::Record* ObjectEffects::find(std::int16_t object, const Key& key) {
    const auto records = objects_.find(object);
    if (records == objects_.end()) {
        return nullptr;
    }
    const auto record = records->second.find(key);
    return record == records->second.end() ? nullptr : &record->second;
}

void ObjectEffects::start(std::int16_t object, const std::string& name, const std::string& bone, int number, Kind kind, bool keep, bool hard, const PlaceOf& placeOf) {
    Key key{name, bone, number};
    Record* record = find(object, key);
    if (!record) {
        const Where where = placeOf(object, bone);
        if (where.state == Where::State::NoBone || where.state == Where::State::Gone) {
            return;
        }
        auto effect = player_.create(name);
        if (!effect) {
            return;
        }
        if (where.state == Where::State::Here) {
            effect->place(where.place);
        }
        effect->setHidden(where.hidden || where.state != Where::State::Here);
        record = &(objects_[object][std::move(key)] = Record{std::move(effect), kind, keep});
    }
    record->effect->start(hard);
}

void ObjectEffects::setPicture(std::int16_t object, const std::string& name, const std::string& bone, int number, const std::string& picture, int firstFrame) {
    if (Record* record = find(object, Key{name, bone, number})) {
        record->effect->setPicture(picture);
        record->effect->setFirstFrame(firstFrame);
    }
}

// What is playing outranks anything a script can ask for.
static constexpr int kPlayingPriority = 100000;

bool ObjectEffects::startInTurn(std::int16_t object, Queued& queued, const PlaceOf& placeOf) {
    const auto& [name, bone, number] = queued.key;
    start(object, name, bone, number, QueuedPrimitive, queued.keep, queued.hard, placeOf);
    Record* record = find(object, queued.key);
    if (record == nullptr) {
        return false;
    }
    if (!queued.picture.empty()) {
        setPicture(object, name, bone, number, queued.picture, queued.firstFrame);
    }
    queued.effect = record->effect;
    queued.priority = kPlayingPriority;
    return true;
}

void ObjectEffects::startQueued(std::int16_t object, const std::string& name, const std::string& bone, int number, bool keep, bool hard, int priority, std::uint64_t expires,
                                const std::string& picture, int firstFrame, const PlaceOf& placeOf) {
    auto& queue = queues_[object];
    Queued queued{Key{name, bone, number}, keep, hard, priority, expires, picture, firstFrame, nullptr};
    if (queue.empty() && !startInTurn(object, queued, placeOf)) {
        queues_.erase(object);
        return;
    }
    queue.push_back(std::move(queued));
    std::push_heap(queue.begin(), queue.end());
}

void ObjectEffects::advanceQueues(std::uint64_t now, const PlaceOf& placeOf) {
    for (auto it = queues_.begin(); it != queues_.end();) {
        auto& queue = it->second;
        const auto takeFront = [&queue] {
            std::pop_heap(queue.begin(), queue.end());
            Queued front = std::move(queue.back());
            queue.pop_back();
            return front;
        };
        if (!queue.empty() && (!queue.front().effect || !queue.front().effect->running())) {
            takeFront();
            while (!queue.empty()) {
                Queued next = takeFront();
                if (now <= next.expires && startInTurn(it->first, next, placeOf)) {
                    queue.push_back(std::move(next));
                    std::push_heap(queue.begin(), queue.end());
                    break;
                }
            }
        }
        it = queue.empty() ? queues_.erase(it) : std::next(it);
    }
}

void ObjectEffects::clearQueue(std::int16_t object) { queues_.erase(object); }

void ObjectEffects::startOnce(std::int16_t object, const std::string& name, const std::string& bone, const PlaceOf& placeOf) {
    const Where where = placeOf(object, bone);
    if (where.state != Where::State::Here) {
        return;
    }
    if (const auto effect = player_.create(name)) {
        effect->place(where.place);
        effect->setHidden(where.hidden);
        effect->start(false);
    }
}

void ObjectEffects::stop(std::int16_t object, const std::string& name, const std::string& bone, int number, bool hard) {
    const Key key{name, bone, number};
    if (Record* record = find(object, key)) {
        record->effect->stop(hard);
        if (hard) {
            objects_[object].erase(key);
        }
    }
}

void ObjectEffects::stopAll(std::int16_t object, Kind kind, bool hard) {
    const auto records = objects_.find(object);
    if (records == objects_.end()) {
        return;
    }
    std::erase_if(records->second, [&](auto& entry) {
        Record& record = entry.second;
        if (record.keep || !record.effect->running() || (kind != AnyKind && record.kind != kind)) {
            return false;
        }
        record.effect->stop(hard);
        return hard;
    });
}

void ObjectEffects::follow(const PlaceOf& placeOf) {
    for (auto objectIt = objects_.begin(); objectIt != objects_.end();) {
        auto& records = objectIt->second;
        for (auto it = records.begin(); it != records.end();) {
            const PlayingEffect& effect = *it->second.effect;
            // The game keeps a finished effect's record until the next save. Starting it again then makes it anew.
            if (!effect.running()) {
                it = records.erase(it);
                continue;
            }
            const Where where = placeOf(objectIt->first, std::get<1>(it->first));
            if (where.state == Where::State::Gone || where.state == Where::State::NoBone) {
                it->second.effect->stop(true);
                it = records.erase(it);
                continue;
            }
            const bool here = where.state == Where::State::Here;
            it->second.effect->setHidden(where.hidden || !here);
            if (here && !where.hidden) {
                it->second.effect->place(where.place);
            }
            ++it;
        }
        objectIt = records.empty() ? objects_.erase(objectIt) : std::next(objectIt);
    }
}

std::size_t ObjectEffects::recordCount() const {
    std::size_t n = 0;
    for (const auto& [object, records] : objects_) {
        n += records.size();
    }
    return n;
}

void appendEffectSprites(const EffectPlayer& player, const ParticleView& view, const std::function<int(const std::string&)>& imageOf, std::vector<ParticleVertex>& vertices,
                         std::vector<render::ParticleBatch>& batches) {
    using Block = effects::ParticleBlock;
    player.forEachParticleSystem([&](const Block& block, const ParticleEmitter& emitter, const EmitterPlace& parent) {
        if (emitter.particles().empty() || (block.flags & Block::kModel)) {
            return;
        }
        const int image = imageOf(emitter.texture());
        if (image < 0) {
            return;
        }
        render::ParticleBatch batch;
        batch.image = image;
        batch.draw = block.blend;
        batch.layer = particleDrawLayer(block);
        batch.firstVertex = static_cast<std::uint32_t>(vertices.size());
        appendParticleQuads(block, emitter, parent, view, vertices);
        batch.vertices = static_cast<std::uint32_t>(vertices.size()) - batch.firstVertex;
        batches.push_back(batch);
    });
    std::stable_sort(batches.begin(), batches.end(), [](const render::ParticleBatch& a, const render::ParticleBatch& b) { return a.layer < b.layer; });
}

} // namespace ts2::scene
