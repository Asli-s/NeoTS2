#pragma once

#include "formats/luascript.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

struct lua_State;

namespace ts2::sim {

class World;
class CodeSource;

// What a running script may read and change: set up by the Lua primitive for one call.
struct ScriptCall {
    std::array<std::int16_t, 3> params{};
    bool hasParams = false;
    std::array<std::int16_t, 8>* temps = nullptr;
    std::vector<std::int16_t>* locals = nullptr;
    std::vector<std::int16_t>* treeParams = nullptr;
    std::int16_t stackObject = 0;
    std::int16_t runningObject = 0;    // the object whose thread runs the script
    std::int16_t runningOnObject = 0;  // the object whose code runs (see Frame::runningOnId)
    World* world = nullptr;
    CodeSource* code = nullptr;
    std::uint32_t codeOwner = 0;  // the group whose private constants the running tree uses
    std::int16_t* globals = nullptr;  // the simulator globals
    std::size_t globalCount = 0;

    bool returnValue = false;  // set by SetScriptReturnValue
    // How the script ended when it didn't simply return.
    std::int16_t errorCode = 0;   // the game's error number for a bad index, 0 for none
    std::string unsupported;      // an API function the engine doesn't have yet
    std::string luaError;         // any other Lua error
};

// The game's Lua 5.0 state: shared modules run once, and every object script is a global
// function named after its resource.
class LuaRuntime {
public:
    LuaRuntime();
    ~LuaRuntime();
    LuaRuntime(const LuaRuntime&) = delete;
    LuaRuntime& operator=(const LuaRuntime&) = delete;

    // Runs the modules and defines the scripts, in the order given; modules can look up object
    // types and constants through `code`. Problems are collected.
    void load(const std::vector<luascript::Script>& modules, const std::vector<luascript::Script>& scripts,
              CodeSource* code = nullptr);
    bool hasScript(const std::string& name);

    // Calls script `name`. False when it couldn't run to the end (see the call's fields).
    bool run(const std::string& name, ScriptCall& call);

    // The game's property set scripts read and write with nGameState.Get/SetProperty, by name.
    using Property = std::variant<bool, std::int32_t, std::uint32_t, float, std::string>;
    std::map<std::string, Property> properties;
    // The installed packs, as nGameState.IsEPInstalled tests them; 0 when unknown.
    std::uint32_t installedPacks = 0;

    std::vector<std::string> problems;

private:
    lua_State* L_ = nullptr;
};

} // namespace ts2::sim
