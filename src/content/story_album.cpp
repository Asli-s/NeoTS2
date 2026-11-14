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

// Text as a CDATA section, as the game writes captions and titles. A "]]>" inside is split across
// two sections.
std::string cdata(std::string_view text) {
    std::string out = "<![CDATA[";
    std::size_t at = 0;
    for (std::size_t end; (end = text.find("]]>", at)) != std::string_view::npos; at = end + 2) {
        out.append(text.substr(at, end + 2 - at));
        out += "]]><![CDATA[";
    }
    out.append(text.substr(at));
    out += "]]>";
    return out;
}

// Plain element text with &, < and > written as entities.
std::string escaped(std::string_view text) {
    std::string out;
    for (const char c : text) {
        out += c == '&' ? "&amp;" : c == '<' ? "&lt;" : c == '>' ? "&gt;" : std::string(1, c);
    }
    return out;
}

// The start of the line holding `at`.
std::size_t lineStart(std::string_view xml, std::size_t at) {
    const std::size_t nl = xml.rfind('\n', at == 0 ? 0 : at - 1);
    return nl == std::string_view::npos || at == 0 ? 0 : nl + 1;
}

// Past the end of the line holding `at` (its line ending too).
std::size_t lineEnd(std::string_view xml, std::size_t at) {
    const std::size_t nl = xml.find('\n', at);
    return nl == std::string_view::npos ? xml.size() : nl + 1;
}

// `head` with the first <tag>...</tag>'s text replaced by `text` (already written as it goes in).
std::string replaced(std::string head, std::string_view tag, const std::string& text) {
    const std::string open = "<" + std::string(tag) + ">", close = "</" + std::string(tag) + ">";
    const std::size_t a = head.find(open);
    const std::size_t b = a == std::string::npos ? std::string::npos : head.find(close, a + open.size());
    if (b != std::string::npos) {
        head.replace(a + open.size(), b - a - open.size(), text);
    }
    return head;
}

} // namespace

StoryAlbum parseStoryAlbum(std::string_view xml) {
    StoryAlbum album;
    album.newline = xml.find("\r\n") != std::string_view::npos || xml.empty() ? "\r\n" : "\n";
    std::size_t entriesStart = xml.find("<Entry>");
    // The album's own fields come before its first entry, as entries have a Title of their own.
    const std::string_view head = xml.substr(0, entriesStart == std::string_view::npos ? xml.size() : entriesStart);
    if (auto v = element(head, "NeighborhoodName")) {
        album.neighborhood = textOf(*v);
    }
    if (auto v = element(head, "FamilyId")) {
        album.familyId = number(*v);
    }
    if (auto v = element(head, "LotID")) {
        album.lotId = number(*v);
    }
    if (auto v = element(head, "Title")) {
        album.title = textOf(*v);
    }
    if (auto v = element(head, "Style")) {
        album.style = textOf(*v);
    }
    std::size_t at = entriesStart == std::string_view::npos ? xml.size() : entriesStart;
    std::size_t lastEnd = std::string_view::npos;
    while (at < xml.size()) {
        std::size_t end = 0;
        const auto entry = element(xml, "Entry", at, &end);
        if (!entry) {
            break;
        }
        at = end;
        lastEnd = end;
        StoryEntry e;
        if (auto v = element(*entry, "Title")) {
            e.title = textOf(*v);
        }
        if (auto v = element(*entry, "ThumbnailFilename")) {
            e.thumbnail = textOf(*v);
        }
        if (auto v = element(*entry, "Snapshot")) {
            e.snapshot = textOf(*v);
        }
        if (auto v = element(*entry, "Text")) {
            e.text = textOf(*v);
        }
        if (auto v = element(*entry, "TextResourceId")) {
            e.textResource = number(*v);
        }
        if (auto v = element(*entry, "TimeStamp")) {
            e.timeStamp = textOf(*v);
        }
        // A file name is a plain name in the Storytelling folder, never a path.
        auto plain = [](const std::string& name) { return name.find_first_of("/\\:") == std::string::npos && name.find("..") == std::string::npos; };
        if (!e.snapshot.empty() && plain(e.snapshot) && plain(e.thumbnail)) {
            album.entries.push_back(std::move(e));
        }
    }
    // The rest of the file around the entries: before the first entry's line and after the last's,
    // or, with none, around the line where they go (before the keywords or the album's end).
    if (lastEnd != std::string_view::npos) {
        album.head = std::string(xml.substr(0, lineStart(xml, entriesStart)));
        album.tail = std::string(xml.substr(lineEnd(xml, lastEnd)));
    } else if (const std::size_t where = std::min(xml.find("<KeywordList"), xml.find("</Album>")); where != std::string_view::npos) {
        album.head = std::string(xml.substr(0, lineStart(xml, where)));
        album.tail = std::string(xml.substr(lineStart(xml, where)));
    }
    return album;
}

std::string writeStoryAlbum(const StoryAlbum& album, std::uint32_t albumId) {
    const std::string& nl = album.newline;
    std::string head = album.head, tail = album.tail;
    if (head.empty()) {
        head = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>" + nl + "<StoryCreator>" + nl + "  <ExchangeMetadata>" + nl + "    <Album>" + nl +
               std::format("      <ID>0x{:08x}</ID>", albumId) + nl + "      <FamilyName><![CDATA[]]></FamilyName>" + nl +
               std::format("      <FamilyId>{}</FamilyId>", album.familyId) + nl + "      <LinkName></LinkName>" + nl +
               "      <NeighborhoodName>" + cdata(album.neighborhood) + "</NeighborhoodName>" + nl +
               std::format("      <LotID>0x{:08x}</LotID>", album.lotId) + nl + "      <LotName><![CDATA[]]></LotName>" + nl +
               "      <Title><![CDATA[]]></Title>" + nl + "      <Style>Album</Style>" + nl;
        tail = "      <KeywordList/>" + nl + "      <UploadDescription><![CDATA[]]></UploadDescription>" + nl + "    </Album>" + nl +
               "  </ExchangeMetadata>" + nl + "</StoryCreator>";
    }
    std::string out = replaced(replaced(head, "Title", cdata(album.title)), "Style", escaped(album.style));
    for (const auto& e : album.entries) {
        out += "      <Entry>" + nl;
        out += "        <Title>" + cdata(e.title) + "</Title>" + nl;
        out += "        <ThumbnailFilename>" + escaped(e.thumbnail) + "</ThumbnailFilename>" + nl;
        out += "        <Snapshot>" + escaped(e.snapshot) + "</Snapshot>" + nl;
        out += "        <Text>" + cdata(e.text) + "</Text>" + nl;
        out += std::format("        <TextResourceId>0x{:08x}</TextResourceId>", e.textResource) + nl;
        out += "        <TimeStamp>" + escaped(e.timeStamp) + "</TimeStamp>" + nl;
        out += "      </Entry>" + nl;
    }
    return out + tail;
}

std::filesystem::path storyAlbumFile(const std::filesystem::path& storytelling, std::uint32_t albumId) {
    return storytelling / std::format("webentry_{:08x}.xml", albumId);
}

std::string storyTimeStamp(std::time_t when) {
    std::tm t{};
#ifdef _WIN32
    localtime_s(&t, &when);
#else
    localtime_r(&when, &t);
#endif
    return std::format("{}/{}/{} {}:{:02}:{:02}", t.tm_mon + 1, t.tm_mday, t.tm_year + 1900, t.tm_hour, t.tm_min, t.tm_sec);
}

} // namespace ts2::content
