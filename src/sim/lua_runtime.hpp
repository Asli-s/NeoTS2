#pragma once

#include "formats/luascript.hpp"

#include <array>
#include <cstddef>
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

    // Set by SetScriptReturnValue. A script that never calls it is true (PercentCalculate, which
    // every "Percentage" tree runs, is one).
    bool returnValue = true;
    std::uint64_t instructions = 0;  // Lua instructions run so far, counted to stop endless loops
    // How the script ended when it didn't simply return.
    std::int16_t errorCode = 0;   // the game's error number for a bad index, 0 for none
    std::string unsupported;      // an API function the engine doesn't have yet
    std::string luaError;         // any other Lua error
};

// A script function the game registers for its Lua scripts, and whether the engine has it.
// Class methods are named "Class:method", functions of plain tables "Table.function".
struct ScriptFunctionInfo {
    std::string name;
    bool implemented = false;
};

// Every script function the game registers, implemented or not yet.
std::vector<ScriptFunctionInfo> scriptFunctions();

// The game's Lua 5.0 state: shared modules run once, and every object script is a global
// function named after its resource.
class LuaRuntime {
public:
    LuaRuntime();
    ~LuaRuntime();
    LuaRuntime(const LuaRuntime&) = delete;
    LuaRuntime& operator=(const LuaRuntime&) = delete;

    // Runs the modules and defines the scripts, in the order given. Modules can look up object
    // types and constants through `code`. Problems are collected.
    void load(const std::vector<luascript::Script>& modules, const std::vector<luascript::Script>& scripts,
              CodeSource* code = nullptr);
    bool hasScript(const std::string& name);

    // Calls script `name`. False when it couldn't run to the end (see the call's fields).
    [[nodiscard]] bool run(const std::string& name, ScriptCall& call);
    // Calls the script kept as global `name`, first compiling it from Lua source text: the game
    // runs a few scripts straight from their string table entry's description.
    [[nodiscard]] bool runSource(const std::string& name, const std::string& source, ScriptCall& call);

    // The game's property set scripts read and write with nGameState.Get/SetProperty, by name.
    using Property = std::variant<bool, std::int32_t, std::uint32_t, float, std::string>;
    std::map<std::string, Property> properties;
    // The installed packs, as nGameState.IsEPInstalled tests them, 0 when unknown.
    std::uint32_t installedPacks = 0;

    std::vector<std::string> problems;
    // Scripts that raised an object error (the game logs them and goes on): how many, and the last
    // one's name and error code.
    std::size_t scriptErrors = 0;
    std::string lastScriptError;

private:
    bool callTop(ScriptCall& call);
    lua_State* L_ = nullptr;
};

} // namespace ts2::sim
