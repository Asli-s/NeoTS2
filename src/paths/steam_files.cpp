#include "paths/steam_files.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>

namespace ts2::paths {

namespace {

// Steam's files are a few kilobytes. These limits keep a broken or hostile file from using much
// memory or time, and the depth limit keeps the reader's recursion shallow.
constexpr std::size_t kMaxText = 1 << 20;
constexpr std::size_t kMaxNodes = 100000;
constexpr int kMaxDepth = 16;

struct Node {
    std::string key;
    std::string value;
    std::vector<Node> children;
    bool isBlock = false;
};

enum class Token { Text, Open, Close, End, Bad };

// Steam's text format: quoted or bare strings, braces around blocks, and // comments.
class Reader {
public:
    explicit Reader(std::string_view text) : text_(text) {}

    Token next(std::string& out) {
        skipSpaceAndComments();
        if (at_ >= text_.size()) {
            return Token::End;
        }
        const char c = text_[at_];
        if (c == '{') {
            ++at_;
            return Token::Open;
        }
        if (c == '}') {
            ++at_;
            return Token::Close;
        }
        out.clear();
        if (c == '"') {
            return quoted(out);
        }
        while (at_ < text_.size() && !isSpace(text_[at_]) && text_[at_] != '{' && text_[at_] != '}' && text_[at_] != '"') {
            out.push_back(text_[at_++]);
        }
        return Token::Text;
    }

private:
    static bool isSpace(char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; }

    void skipSpaceAndComments() {
        while (at_ < text_.size()) {
            if (isSpace(text_[at_])) {
                ++at_;
            } else if (text_.compare(at_, 2, "//") == 0) {
                while (at_ < text_.size() && text_[at_] != '\n') {
                    ++at_;
                }
            } else {
                return;
            }
        }
    }

    // A quoted string. A backslash escapes the next character, so "C:\\Games" is C:\Games.
    Token quoted(std::string& out) {
        ++at_;
        while (at_ < text_.size()) {
            char c = text_[at_++];
            if (c == '"') {
                return Token::Text;
            }
            if (c == '\\' && at_ < text_.size()) {
                c = text_[at_++];
                if (c == 'n') {
                    c = '\n';
                } else if (c == 't') {
                    c = '\t';
                }
            }
            out.push_back(c);
        }
        return Token::Bad;
    }

    std::string_view text_;
    std::size_t at_ = 0;
};

// Reads key/value pairs and blocks until the block's closing brace (or the end at depth 0).
bool readBlock(Reader& reader, int depth, std::size_t& nodes, std::vector<Node>& out) {
    if (depth > kMaxDepth) {
        return false;
    }
    std::string text;
    while (true) {
        const Token token = reader.next(text);
        if (token == Token::End) {
            return depth == 0;
        }
        if (token == Token::Close) {
            return depth > 0;
        }
        if (token != Token::Text || ++nodes > kMaxNodes) {
            return false;
        }
        Node node;
        node.key = std::move(text);
        const Token after = reader.next(text);
        if (after == Token::Text) {
            node.value = std::move(text);
        } else if (after == Token::Open) {
            node.isBlock = true;
            if (!readBlock(reader, depth + 1, nodes, node.children)) {
                return false;
            }
        } else {
            return false;
        }
        out.push_back(std::move(node));
    }
}

std::optional<std::vector<Node>> parse(std::string_view text) {
    if (text.size() > kMaxText) {
        return std::nullopt;
    }
    Reader reader(text);
    std::size_t nodes = 0;
    std::vector<Node> top;
    if (!readBlock(reader, 0, nodes, top)) {
        return std::nullopt;
    }
    return top;
}

bool sameKey(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

bool allDigits(std::string_view s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}

const Node* findBlock(const std::vector<Node>& nodes, std::string_view key) {
    for (const Node& node : nodes) {
        if (node.isBlock && sameKey(node.key, key)) {
            return &node;
        }
    }
    return nullptr;
}

} // namespace

std::vector<std::string> steamLibraryFolders(std::string_view vdfText) {
    std::vector<std::string> folders;
    const auto top = parse(vdfText);
    if (!top) {
        return folders;
    }
    const Node* list = findBlock(*top, "libraryfolders");
    if (!list) {
        return folders;
    }
    // Libraries are numbered entries. Newer Steam files hold a block with a "path" in each, older
    // ones the folder itself.
    for (const Node& entry : list->children) {
        if (!allDigits(entry.key)) {
            continue;
        }
        if (!entry.isBlock) {
            if (!entry.value.empty()) {
                folders.push_back(entry.value);
            }
            continue;
        }
        for (const Node& field : entry.children) {
            if (!field.isBlock && sameKey(field.key, "path") && !field.value.empty()) {
                folders.push_back(field.value);
                break;
            }
        }
    }
    return folders;
}

std::optional<std::string> steamInstallFolder(std::string_view manifestText) {
    const auto top = parse(manifestText);
    if (!top) {
        return std::nullopt;
    }
    const Node* state = findBlock(*top, "AppState");
    if (!state) {
        return std::nullopt;
    }
    for (const Node& field : state->children) {
        if (field.isBlock || !sameKey(field.key, "installdir")) {
            continue;
        }
        // Only a single folder name, so a broken manifest can't point outside the library.
        const std::string& name = field.value;
        if (name.empty() || name == "." || name == ".." ||
            name.find_first_of("/\\:") != std::string::npos) {
            return std::nullopt;
        }
        return name;
    }
    return std::nullopt;
}

} // namespace ts2::paths
