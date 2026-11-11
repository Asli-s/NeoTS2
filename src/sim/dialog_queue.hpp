#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace ts2::sim {

// Requests for a dialog that waits for the player, shown one at a time in the order made, one per
// asking object. `Request` has the asker's object id in `asker`.
template <class Request, class Answer>
class DialogQueue {
public:
    // Asks anew for the asker, closing its earlier request. One still waiting or answered is kept.
    void ask(Request request) {
        const auto own = find(request.asker);
        if (own != entries_.end()) {
            if (own->state != State::Shown) {
                return;
            }
            entries_.erase(own);
        }
        entries_.push_back({std::move(request), State::Waiting, Answer{}});
        showNext();
    }
    // The request shown, null when none is.
    const Request* shown() const {
        const auto it = std::ranges::find_if(entries_, [](const Entry& e) { return e.state == State::Shown; });
        return it != entries_.end() ? &it->request : nullptr;
    }
    // Answers the shown request and shows the next.
    void answer(Answer answer) {
        const auto it = std::ranges::find_if(entries_, [](const Entry& e) { return e.state == State::Shown; });
        if (it == entries_.end()) {
            return;
        }
        it->state = State::Answered;
        it->answer = std::move(answer);
        showNext();
    }
    // The asker's answer once given, `none` when the asker has no request. Nothing while it waits.
    std::optional<Answer> take(std::int16_t asker, Answer none) {
        const auto own = find(asker);
        if (own == entries_.end()) {
            return none;
        }
        if (own->state != State::Answered) {
            return std::nullopt;
        }
        Answer answer = std::move(own->answer);
        entries_.erase(own);
        return answer;
    }
    // Drops the asker's request, as when the asker leaves the lot.
    void forget(std::int16_t asker) {
        std::erase_if(entries_, [&](const Entry& e) { return e.request.asker == asker; });
        showNext();
    }

private:
    enum class State { Waiting, Shown, Answered };
    struct Entry {
        Request request;
        State state = State::Waiting;
        Answer answer{};
    };
    std::vector<Entry> entries_;  // in the order asked

    auto find(std::int16_t asker) {
        return std::ranges::find_if(entries_, [&](const Entry& e) { return e.request.asker == asker; });
    }
    void showNext() {
        if (std::ranges::any_of(entries_, [](const Entry& e) { return e.state == State::Shown; })) {
            return;
        }
        const auto next = std::ranges::find_if(entries_, [](const Entry& e) { return e.state == State::Waiting; });
        if (next != entries_.end()) {
            next->state = State::Shown;
        }
    }
};

} // namespace ts2::sim
