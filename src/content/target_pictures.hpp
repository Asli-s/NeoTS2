#pragma once

#include "formats/tga.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <map>
#include <optional>
#include <thread>
#include <vector>

namespace ts2::content {

// Reads a Sim's portrait (image `instance`) from its character file, or nullopt when it can't. Runs on
// worker threads that may outlive the TargetPictures, so it must not touch shared state and must
// own (not reference) everything it captures.
using PortraitReader = std::function<std::optional<tga::Pixels>(const std::filesystem::path& file, std::uint32_t instance)>;

// The reader the game's files need: opens the character file on its own and decodes the picture.
[[nodiscard]] std::optional<tga::Pixels> readCharacterPortrait(const std::filesystem::path& file, std::uint32_t instance);

// Pictures of Sims by neighbor id (those wants are aimed at, those in the relationships panel): the
// middle of each Sim's portrait at one size (want icons' by default). Pictures from character files
// are read and made in the background, so the frame never waits on the disk. Until one arrives the
// want shows without it.
class TargetPictures {
public:
    static constexpr std::uint32_t kPictureSize = 44;
    static constexpr std::size_t kMaxLoading = 4;

    explicit TargetPictures(PortraitReader reader = readCharacterPortrait, std::uint32_t size = kPictureSize)
        : reader_(std::move(reader)), size_(size) {}
    // Never waits: loads still running finish on their own.
    ~TargetPictures();
    TargetPictures(const TargetPictures&) = delete;
    TargetPictures& operator=(const TargetPictures&) = delete;

    // The picture of a neighbor, or null while it loads or when it has none.
    const tga::Pixels* find(std::int16_t neighbor) const;
    // Whether the neighbor's picture is known (made, failed or loading).
    bool known(std::int16_t neighbor) const { return ready_.count(neighbor) != 0 || loading_.count(neighbor) != 0; }
    std::size_t loading() const { return loading_.size(); }

    // Makes a neighbor's picture from a portrait at hand (a Sim drawn on the lot).
    void make(std::int16_t neighbor, const tga::Pixels& portrait);
    // Starts reading a neighbor's picture from its character file. False when kMaxLoading are
    // loading already or no thread can be started (ask again on a later frame), or it is known
    // already.
    bool load(std::int16_t neighbor, const std::filesystem::path& characterFile, std::uint32_t instance);
    // Takes in the pictures that have finished loading. True when any did.
    bool collect();
    // Forgets every picture (a new household). Loads still running finish on their own.
    void clear();

private:
    // A load: its own thread, joined once its result is in, so everything it used is freed then.
    struct Job {
        std::future<std::optional<tga::Pixels>> result;
        std::thread thread;
    };
    void joinFinishedAbandoned();

    PortraitReader reader_;

    std::uint32_t size_ = kPictureSize;
    std::map<std::int16_t, std::optional<tga::Pixels>> ready_;
    std::map<std::int16_t, Job> loading_;
    std::vector<Job> abandoned_;  // kept until done, so clear() never waits
};

} // namespace ts2::content
