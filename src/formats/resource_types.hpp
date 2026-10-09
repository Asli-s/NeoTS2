#pragma once

#include <cstdint>
#include <string>

namespace ts2::types {

constexpr std::uint32_t kDirectory = 0xE86B1EEF;
constexpr std::uint32_t kBehavior  = 0x42484156;
constexpr std::uint32_t kStringTable = 0x53545223;
constexpr std::uint32_t kObjectDefinition = 0x4F424A44;
constexpr std::uint32_t kObjectTypeTable = 0x6F626A74;
constexpr std::uint32_t kObjectMap = 0x4F626A4D;
constexpr std::uint32_t kSavedObject = 0x584F424A;
constexpr std::uint32_t kSavedObjectWithMaterial = 0x584D544F;
constexpr std::uint32_t kSavedPerson = 0x50455253;
constexpr std::uint32_t kWallGraph = 0x0A284D0B;
constexpr std::uint32_t kArray3D = 0x2A51171B;
constexpr std::uint32_t kPatternMap = 0xCAC4FC40;
constexpr std::uint32_t kWallLayer = 0x8A84D7B0;
constexpr std::uint32_t kFencePostLayer = 0xAB4BA572;
constexpr std::uint32_t kRoofs = 0xAB9406AA;
constexpr std::uint32_t kNameMap = 0x4E6D6150;
constexpr std::uint32_t kSceneObject = 0xFA1C39F7;
constexpr std::uint32_t kSimDescription = 0xAACE2EFB;
constexpr std::uint32_t kSimRelationship = 0xCC364C2A;
constexpr std::uint32_t kFamily = 0x46414D49;
constexpr std::uint32_t kFamilyTies = 0x8C870743;
constexpr std::uint32_t kSlots = 0x534C4F54;
constexpr std::uint32_t kWantsAndFears = 0xCD95548E;
constexpr std::uint32_t kWantDefinition = 0xED7D7B4D;
constexpr std::uint32_t kInteractionTable = 0x54544142;
constexpr std::uint32_t kLuaModule = 0x9012468A;
constexpr std::uint32_t kLuaScript = 0x9012468B;
constexpr std::uint32_t kNeighborhoodRecord = 0x4E474248;  // NGBH
constexpr std::uint32_t kDataTables = 0x3053CF74;

std::string typeName(std::uint32_t type);

} // namespace ts2::types
