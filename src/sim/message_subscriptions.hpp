#pragma once

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <string>
#include <string_view>

namespace ts2::sim {

// The messages objects subscribed to with nMessaging.SubscribeMessage: for each message id, the
// objects that hear it and the name of the tree each one runs when it comes.
class MessageSubscriptions {
public:
    // The game keeps no limit. These bound what a runaway script can pile up. A name longer than a
    // tree's name can hold matches no tree, so keeping only its start changes nothing.
    static constexpr std::size_t kMaxSubscriptions = 1 << 16;
    static constexpr std::size_t kMaxNameLength = 255;

    // An object subscribing again to the same message replaces its tree name. False when the limit
    // is reached.
    bool subscribe(std::uint32_t message, std::int16_t object, std::string_view tree) {
        auto& objects = byMessage_[message];
        const auto [it, added] = objects.try_emplace(object);
        if (added && count_ >= kMaxSubscriptions) {
            objects.erase(it);
            if (objects.empty()) {
                byMessage_.erase(message);
            }
            return false;
        }
        count_ += added ? 1 : 0;
        it->second.assign(tree.substr(0, kMaxNameLength));
        return true;
    }

    // A deleted object's subscriptions go with it.
    void dropObject(std::int16_t object) {
        for (auto it = byMessage_.begin(); it != byMessage_.end();) {
            count_ -= it->second.erase(object);
            it = it->second.empty() ? byMessage_.erase(it) : std::next(it);
        }
    }

    // The objects subscribed to `message` and their tree names, or null.
    const std::map<std::int16_t, std::string>* subscribers(std::uint32_t message) const {
        const auto it = byMessage_.find(message);
        return it != byMessage_.end() ? &it->second : nullptr;
    }

    std::size_t size() const { return count_; }

private:
    std::map<std::uint32_t, std::map<std::int16_t, std::string>> byMessage_;
    std::size_t count_ = 0;
};

} // namespace ts2::sim
