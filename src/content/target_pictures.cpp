#include "content/target_pictures.hpp"

#include "content/ui_meters.hpp"
#include "formats/dbpf.hpp"
#include "formats/image.hpp"
#include "formats/resource_types.hpp"

#include <chrono>
#include <system_error>

namespace ts2::content {

namespace {

// The middle of a portrait at want icon size; nullopt for a broken or oversized one.
std::optional<tga::Pixels> picture(const tga::Pixels& portrait) {
    if (portrait.width > kMaxPortraitSide || portrait.height > kMaxPortraitSide) {
        return std::nullopt;
    }
    auto out = scaled(portraitCrop(portrait), TargetPictures::kPictureSize, TargetPictures::kPictureSize);
    return out.rgba.empty() ? std::nullopt : std::optional<tga::Pixels>(std::move(out));
}

bool done(const std::future<std::optional<tga::Pixels>>& f) {
    return f.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
}

// What a load makes, on its thread: the reader's portrait as a want picture.
std::optional<tga::Pixels> loadPicture(const PortraitReader& reader, const std::filesystem::path& file, std::uint32_t instance) {
    try {
        const auto portrait = reader(file, instance);
        return portrait ? picture(*portrait) : std::nullopt;
    } catch (...) {
        return std::nullopt;  // a broken file only costs its picture
    }
}

} // namespace

std::optional<tga::Pixels> readCharacterPortrait(const std::filesystem::path& file, std::uint32_t instance) {
    dbpf::Package package{file};
    for (const auto& e : package.entries()) {
        if (e.key.type == types::kImage && e.key.instance == instance) {
            return image::decode(package.read(e), kMaxPortraitSide);
        }
    }
    return std::nullopt;
}

const tga::Pixels* TargetPictures::find(std::int16_t neighbor) const {
    const auto it = ready_.find(neighbor);
    return it != ready_.end() && it->second ? &*it->second : nullptr;
}

void TargetPictures::make(std::int16_t neighbor, const tga::Pixels& portrait) { ready_[neighbor] = picture(portrait); }

bool TargetPictures::load(std::int16_t neighbor, const std::filesystem::path& characterFile, std::uint32_t instance) {
    joinFinishedAbandoned();
    // Loads left running by clear() count too, so threads can't pile up.
    if (loading_.size() + abandoned_.size() >= kMaxLoading || known(neighbor)) {
        return false;
    }
    // Its place first, so a thread that starts always has an owner that joins it.
    Job& job = loading_[neighbor];
    std::promise<std::optional<tga::Pixels>> promise;
    job.result = promise.get_future();
    try {
        // The thread gets its own copies of what it needs.
        job.thread = std::thread([promise = std::move(promise), reader = reader_, characterFile, instance]() mutable {
            promise.set_value(loadPicture(reader, characterFile, instance));
        });
    } catch (const std::system_error&) {
        loading_.erase(neighbor);
        return false;  // no thread to be had now: ask again later
    }
    return true;
}

bool TargetPictures::collect() {
    bool any = false;
    for (auto it = loading_.begin(); it != loading_.end();) {
        if (done(it->second.result)) {
            ready_[it->first] = it->second.result.get();
            it->second.thread.join();
            it = loading_.erase(it);
            any = true;
        } else {
            ++it;
        }
    }
    joinFinishedAbandoned();
    return any;
}

void TargetPictures::joinFinishedAbandoned() {
    std::erase_if(abandoned_, [](Job& job) {
        if (!done(job.result)) {
            return false;
        }
        job.thread.join();
        return true;
    });
}

void TargetPictures::clear() {
    ready_.clear();
    for (auto& [neighbor, job] : loading_) {
        abandoned_.push_back(std::move(job));
    }
    loading_.clear();
}

TargetPictures::~TargetPictures() {
    auto finish = [](Job& job) {
        if (done(job.result)) {
            job.thread.join();
        } else {
            job.thread.detach();  // its thread owns copies of all it uses, so it can outlive us
        }
    };
    for (auto& [neighbor, job] : loading_) {
        finish(job);
    }
    for (Job& job : abandoned_) {
        finish(job);
    }
}

} // namespace ts2::content
