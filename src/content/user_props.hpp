#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

// Reads a file of settings laid out as userProps.xml is, such as the install's globalProps.xml.
// Empty when there is none or it can't be read.
UserProps readPropsFile(const std::filesystem::path& file);

// Reads the user folder's Config/userProps.xml. Empty when there is none or it can't be read.
UserProps readUserProps(const std::filesystem::path& userFolder);

// One settings group of the game's options panels. The group window's caption names it as
// "<kind>:<setting>:<bool or int>:<value of button 1>:<value of button 2>...", such as
// "radio:edgeScrolling:bool:0:1". The buttons inside the group have ids 1, 2 and so on.
struct OptionControl {
    // A radio setting is saved under its own name. A radiooption one picks a setting of a Graphics
    // Rules option and is saved as "option" + its name, holding the setting's number.
    enum class Kind { Radio, RadioOption, Slider };
    Kind kind = Kind::Radio;
    std::string key;
    bool isBool = false;
    std::vector<std::string> values;  // button n's value is values[n - 1]

    [[nodiscard]] std::string savedKey() const;
    // The value as userProps.xml keeps it: "true" or "false" for a radio true or false setting,
    // the number for the rest.
    [[nodiscard]] std::string savedValue(std::size_t button) const;
    // The button (from 1) whose value a saved text holds, nothing when none does.
    [[nodiscard]] std::optional<std::size_t> buttonFor(std::string_view saved) const;
};

// Reads a group's caption. Nothing for other captions, or ones with a setting name or value that
// isn't letters and digits.
std::optional<OptionControl> parseOptionCaption(std::string_view caption);

// Sets a setting in userProps.xml's text: its value when the setting is there, otherwise a new
// element before the file's closing tag. False when the text has no closing tag, or the key or
// value holds anything but letters and digits.
bool setUserProp(std::string& xml, std::string_view key, std::string_view value, bool isBool);

// Saves settings to the user folder's Config/userProps.xml, keeping the rest of the file as it is.
// The new file replaces the old one only once it is fully written. False when it can't be saved.
bool saveUserProps(const std::filesystem::path& userFolder, const std::map<std::string, std::pair<std::string, bool>>& settings);

// NeoTS2's own settings, which the game has none of: Config/neots2Props.xml in the user folder, kept
// the way userProps.xml is. Separate so the game's file holds only what the game knows.
UserProps readNeoProps(const std::filesystem::path& userFolder);
bool saveNeoProps(const std::filesystem::path& userFolder, const std::map<std::string, std::pair<std::string, bool>>& settings);

// The settings Graphics Rules.sgr's option `option` gives for its setting number `setting`: each
// property its "setting" block sets (such as lotSkirtSizeIncrease from setting 0 of
// LotSkirtSizeIncrease) and the value as written. A setting named by a variable ($On) takes the
// number a "seti" line gives it. Empty when the option or setting isn't there.
std::map<std::string, std::string> graphicsOptionProps(std::string_view rules, std::string_view option, int setting);

// The newest Graphics Rules.sgr of an install's products, empty when there is none or it can't be read.
std::string readGraphicsRules(const std::filesystem::path& install);

// What a Graphics Rules option sets as the game starts: its saved setting (userProps.xml's
// "option" + name) looked up in the rules. Nothing when no setting is saved, as the game then
// takes the setting its hardware level picks.
std::optional<std::string> savedOptionProp(const UserProps& saved, std::string_view rules, std::string_view option, std::string_view prop);

} // namespace ts2::content
