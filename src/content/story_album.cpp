#include "content/story_album.hpp"

#include <cstdlib>
#include <format>
#include <optional>

namespace ts2::content {

namespace {

// The text of the first <tag>...</tag> in `xml` from `from` on, its end put in `end`. Empty when
// there is none.
std::optional<std::string_view> element(std::string_view xml, std::string_view tag, std::size_t from = 0, std::size_t* end = nullptr) {
    const std::string open = "<" + std::string(tag) + ">", close = "</" + std::string(tag) + ">";
    const std::size_t a = xml.find(open, from);
    if (a == std::string_view::npos) {
        return std::nullopt;
    }
    const std::size_t b = xml.find(close, a + open.size());
    if (b == std::string_view::npos) {
        return std::nullopt;
    }
    if (end != nullptr) {
        *end = b + close.size();
    }
    return xml.substr(a + open.size(), b - a - open.size());
}

// An element's text: a CDATA section as it is, other text with its entities (&amp; &lt; &gt;
// &quot; &apos;) turned back into characters.
std::string textOf(std::string_view raw) {
    constexpr std::string_view kOpen = "<![CDATA[", kClose = "]]>";
    std::string out;
    std::size_t at = 0;
    while (at < raw.size()) {
        if (raw.substr(at, kOpen.size()) == kOpen) {
            const std::size_t end = raw.find(kClose, at + kOpen.size());
            const std::size_t stop = end == std::string_view::npos ? raw.size() : end;
            out.append(raw.substr(at + kOpen.size(), stop - at - kOpen.size()));
            at = end == std::string_view::npos ? raw.size() : end + kClose.size();
            continue;
        }
        if (raw[at] == '&') {
            static constexpr std::pair<std::string_view, char> kEntities[] = {
                {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}};
            bool known = false;
            for (const auto& [name, c] : kEntities) {
                if (raw.substr(at, name.size()) == name) {
                    out.push_back(c);
                    at += name.size();
                    known = true;
                    break;
                }
            }
            if (known) {
                continue;
            }
        }
        out.push_back(raw[at++]);
    }
    return out;
}

std::uint32_t number(std::string_view text) {
    const std::string s(text);
    return static_cast<std::uint32_t>(std::strtoul(s.c_str(), nullptr, 0));
}

} // namespace

StoryAlbum parseStoryAlbum(std::string_view xml) {
    StoryAlbum album;
    std::size_t entriesStart = xml.find("<Entry>");
    if (entriesStart == std::string_view::npos) {
        entriesStart = xml.size();
    }
    // The album's own fields come before its first entry, as entries have a Title of their own.
    const std::string_view head = xml.substr(0, entriesStart);
    if (auto v = element(head, "NeighborhoodName")) {
        album.neighborhood = textOf(*v);
    }
    if (auto v = element(head, "FamilyId")) {
        album.familyId = number(*v);
    }
    if (auto v = element(head, "Style")) {
        album.style = textOf(*v);
    }
    std::size_t at = entriesStart;
    while (at < xml.size()) {
        std::size_t end = 0;
        const auto entry = element(xml, "Entry", at, &end);
        if (!entry) {
            break;
        }
        at = end;
        StoryEntry e;
        if (auto v = element(*entry, "Snapshot")) {
            e.snapshot = textOf(*v);
        }
        if (auto v = element(*entry, "Text")) {
            e.text = textOf(*v);
        }
        if (auto v = element(*entry, "TextResourceId")) {
            e.textResource = number(*v);
        }
        // A snapshot's name is a plain file name in the Storytelling folder, never a path.
        if (!e.snapshot.empty() && e.snapshot.find_first_of("/\\:") == std::string::npos && e.snapshot.find("..") == std::string::npos) {
            album.entries.push_back(std::move(e));
        }
    }
    return album;
}

std::filesystem::path storyAlbumFile(const std::filesystem::path& storytelling, std::uint32_t albumId) {
    return storytelling / std::format("webentry_{:08x}.xml", albumId);
}

} // namespace ts2::content
