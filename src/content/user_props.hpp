#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace ts2::content {

// The game's options as it saves them in the user folder's Config/userProps.xml: each setting's key
// and its text, such as rotateCameraSims1Mode (the Camera Rotation option) and "true".
struct UserProps {
    std::map<std::string, std::string> values;

    // A true or false setting, nothing when it is missing or holds something else.
    [[nodiscard]] std::optional<bool> flag(std::string_view key) const;
};

// Reads the settings from the file's text. Only single-value settings are kept (not lists such as
// the visited neighborhoods), and a broken file gives what was read before the break.
UserProps parseUserProps(std::string_view xml);

// Reads the user folder's Config/userProps.xml. Empty when there is none or it can't be read.
UserProps readUserProps(const std::filesystem::path& userFolder);

} // namespace ts2::content
