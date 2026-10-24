#include "formats/ngbh.hpp"
#include "sim/code_source.hpp"
#include "sim/lua_runtime.hpp"
#include "sim/world.hpp"

#include "lauxlib.h"
#include "lua.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// The script API driven by the input: a fixed script reads the input as instructions and calls any
// global function or class method (found by walking the globals, so new API is covered too) with
// arguments from a pool of numbers, strings, tables, nil, booleans, what earlier calls returned and
// metatables. It also indexes and assigns into pool values and calls them. Every call is protected,
// so one error doesn't end the run. The world has an object, a Sim and a neighbor with tokens.

namespace {

constexpr const char* kDriver = R"lua(
local pcall, unpack, pairs, ipairs, type, getmetatable = pcall, unpack, pairs, ipairs, type, getmetatable
local mod, getn, insert, sort, byteAt, char = math.mod, table.getn, table.insert, table.sort, string.byte, string.char
local input = nGameState.GetProperty("input") or ""
local length = string.len(input)
local at = 0
local function byte()
    at = at + 1
    if at > length then return 0 end
    return byteAt(input, at)
end

-- Every function in the globals and in the global tables, in name order so runs repeat.
local callables = {}
local function collect(t, nested)
    local names = {}
    for k in pairs(t) do
        if type(k) == "string" then insert(names, k) end
    end
    sort(names)
    for _, k in ipairs(names) do
        local v = t[k]
        if type(v) == "function" then
            if v ~= string.rep and v ~= Driver then insert(callables, v) end  -- rep("", 2^31) spins; Driver is this script
        elseif type(v) == "table" and v ~= t and not nested then
            collect(v, true)
        end
    end
end
collect(_G, false)
local count = getn(callables)

local pool = {}
local added = 0
local function add(v)
    pool[mod(added, 64) + 1] = v
    added = added + 1
end
local function pick() return pool[mod(byte(), 64) + 1] end
for _, v in ipairs({0, 1, -1, 8, 2147483648, 1e300, -0.5, "", "x", {}, true, false}) do add(v) end
-- Names random bytes would hardly ever spell: metatable fields and the globals table.
for _, v in ipairs({"__gc", "__index", "__newindex", "__call", "__metatable", "n", _G}) do add(v) end
add(0 / 0)

local function call(f, first)
    local args = {n = mod(byte(), 5)}
    for i = 1, args.n do args[i] = pick() end
    if first ~= nil then args[1] = first; if args.n == 0 then args.n = 1 end end
    local results = {pcall(f, unpack(args))}
    for i = 2, getn(results) do add(results[i]) end
end

while at < length do
    local op = mod(byte(), 10)
    if op == 0 or op == 1 then
        call(callables[mod(byte(), count) + 1])
    elseif op == 2 then
        call(callables[mod(byte(), count) + 1], pick())  -- as a method of a pool value
    elseif op == 3 then
        call(pick())
    elseif op == 4 then
        add((byte() * 16777216 + byte() * 65536 + byte() * 256 + byte()) - 2147483648)
    elseif op == 5 then
        local s = ""
        for i = 1, mod(byte(), 24) do s = s .. char(byte()) end
        add(s)
    elseif op == 6 then
        local t = {}
        for i = 1, mod(byte(), 6) do pcall(function(k, v) t[k] = v end, pick(), pick()) end
        add(t)
    elseif op == 7 then
        add(getmetatable(pick()))
    elseif op == 8 then
        local ok, v = pcall(function(a, b) return a[b] end, pick(), pick())
        add(v)
    else
        pcall(function(a, b, c) a[b] = c end, pick(), pick(), pick())
    end
end
)lua";

ts2::luascript::Script compile(const char* name, const char* source) {
    lua_State* L = lua_open();
    ts2::luascript::Script script;
    script.name = name;
    if (luaL_loadbuffer(L, source, std::char_traits<char>::length(source), name) == 0) {
        lua_dump(L, [](lua_State*, const void* p, size_t size, void* out) {
            auto* bytes = static_cast<std::vector<std::uint8_t>*>(out);
            bytes->insert(bytes->end(), static_cast<const std::uint8_t*>(p), static_cast<const std::uint8_t*>(p) + size);
            return 0;
        }, &script.chunk);
    }
    lua_close(L);
    return script;
}

constexpr std::uint32_t kGroup = 0x7F000001;

ts2::objd::ObjectDefinition definition(std::uint32_t guid, std::uint16_t type) {
    ts2::objd::ObjectDefinition d;
    d.fields.assign(ts2::objd::kMaxFields, 0);
    d.fields[ts2::objd::field::kGuid1] = static_cast<std::uint16_t>(guid);
    d.fields[ts2::objd::field::kGuid2] = static_cast<std::uint16_t>(guid >> 16);
    d.fields[ts2::objd::field::kType] = type;
    return d;
}

// The two object types of the world, a constants table and a string table.
class Code : public ts2::sim::CodeSource {
public:
    Code() {
        types_.push_back({kGroup, definition(0x6C50BA66, 4)});
        types_.push_back({kGroup, definition(0x51515151, ts2::sim::kPersonType)});
    }
    std::shared_ptr<const ts2::bhav::Behavior> behavior(std::uint32_t, std::uint16_t) override { return nullptr; }
    const std::vector<std::int16_t>* constants(std::uint32_t, std::uint16_t instance) override {
        return instance == 0x1000 ? &constants_ : nullptr;
    }
    std::uint32_t semiGlobalGroup(std::uint32_t group) override { return group; }
    const ts2::sim::ObjectType* objectType(std::uint32_t guid) override {
        for (const auto& t : types_) {
            if (t.definition.guid() == guid) {
                return &t;
            }
        }
        return nullptr;
    }
    std::optional<std::string> string(std::uint32_t, std::uint16_t, std::size_t index) override {
        return index < 2 ? std::optional<std::string>("Script") : std::nullopt;
    }
    std::optional<std::uint16_t> behaviorByName(std::uint32_t, const std::string&) override { return std::nullopt; }
    std::shared_ptr<const ts2::ttab::InteractionTable> interactionTable(std::uint32_t, std::uint16_t) override {
        return nullptr;
    }

private:
    std::vector<ts2::sim::ObjectType> types_;
    std::vector<std::int16_t> constants_{1, -2, 300, 0, 7};
};

std::shared_ptr<ts2::sim::SavedNeighborhood> neighborhood() {
    auto record = std::make_shared<ts2::ngbh::Neighborhood>();
    ts2::xobj::Token token;
    token.guid = 4242;
    token.properties = {1, 2};
    record->neighbors[8].singular = {token};
    token.properties = {3, 0};
    record->neighbors[8].counted = {token};
    auto hood = std::make_shared<ts2::sim::SavedNeighborhood>();
    hood->record = record;
    hood->neighborIds = {8};
    hood->neighbors[8].guid = 0x0D000008;
    hood->neighbors[8].personData.assign(231, 1);
    return hood;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    // Made for each input, so a replay in the tests leaves nothing allocated behind.
    const auto driver = compile("Driver", kDriver);
    Code code;
    const auto hood = neighborhood();

    auto runtime = std::make_shared<ts2::sim::LuaRuntime>();
    runtime->load({}, {driver});
    runtime->properties["input"] = std::string(reinterpret_cast<const char*>(data), size);
    ts2::sim::World world;
    world.setScripts(runtime);
    world.setNeighborhood(hood);
    const auto& object = world.create(kGroup, definition(0x6C50BA66, 4));
    world.create(kGroup, definition(0x51515151, ts2::sim::kPersonType));

    std::array<std::int16_t, 8> temps{};
    std::vector<std::int16_t> locals(4, 0), params(4, 0);
    std::vector<std::int16_t> globals(60, 0);
    ts2::sim::ScriptCall call;
    call.temps = &temps;
    call.locals = &locals;
    call.treeParams = &params;
    call.stackObject = 2;
    call.runningObject = object.id;
    call.runningOnObject = object.id;
    call.world = &world;
    call.code = &code;
    call.codeOwner = kGroup;
    call.globals = globals.data();
    call.globalCount = globals.size();
    (void)runtime->run("Driver", call);
    // The input as script text too, as the game compiles a string table entry's description.
    ts2::sim::ScriptCall text = call;
    text.unsupported.clear();
    text.luaError.clear();
    text.errorCode = 0;
    (void)runtime->runSource("Text_0x7fd46cd0", std::string(reinterpret_cast<const char*>(data), size), text);
    return 0;
}
