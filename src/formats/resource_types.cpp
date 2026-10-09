#include "formats/resource_types.hpp"

#include <cstdio>
#include <unordered_map>

namespace ts2::types {

std::string typeName(std::uint32_t type) {
    static const std::unordered_map<std::uint32_t, std::string> names = {
        {kDirectory, "DIR"},  // list of compressed resources
        {kBehavior,  "BHAV"}, // SimAntics bytecode
        {0x42434F4E, "BCON"}, // behavior constants
        {0x474C4F42, "GLOB"}, // semi-global reference
        {0x4F424A44, "OBJD"}, // object definition
        {0x4F424A66, "OBJf"}, // object functions
        {0x54544142, "TTAB"}, // pie-menu interactions
        {0x54544173, "TTAs"}, // pie-menu strings
        {0x54505250, "TPRP"}, // BHAV parameter names
        {0x54524353, "TRCN"}, // BCON labels
        {0x534C4F54, "SLOT"}, // routing slots
        {0x53545223, "STR#"}, // string table
        {0x43545353, "CTSS"}, // catalog description
        {0x50414C54, "PALT"}, // palette
        {0x53505232, "SPR2"}, // sprite
        {0x44475250, "DGRP"}, // draw group
        {0xE519C933, "CRES"}, // scenegraph resource node
        {0xFC6EB1F7, "SHPE"}, // shape
        {0x7BA3838C, "GMND"}, // geometric node
        {0xAC4F8687, "GMDC"}, // mesh data
        {0x49596978, "TXMT"}, // material definition
        {0x1C4A276C, "TXTR"}, // texture
        {0xED534136, "LIFO"}, // large texture mipmap
        {0xFB00791E, "ANIM"}, // animation
        {0x4C697E5A, "MMAT"}, // material override
        {0xEBCF3E27, "GZPS"}, // property set
        {0x0C560F39, "BINX"}, // catalog entry
        {kSimDescription, "SDSC"}, // a neighbor (one per Sim in the neighborhood)
        {kSimRelationship, "SREL"}, // relationship of one neighbor towards another
        {0x856DDBAC, "IMG"},  // JPEG/PNG image
        {0xFA1C39F7, "cObject"}, // lot object placement
        {0x0A284D0B, "cWallGraph"}, // lot walls and rooms
        {0x2A51171B, "c3DArray"}, // lot grid per tile and level
    };

    if (auto it = names.find(type); it != names.end()) {
        return it->second;
    }

    std::string ascii;
    for (int shift = 24; shift >= 0; shift -= 8) {
        const char c = static_cast<char>((type >> shift) & 0xFF);
        if (c < 0x20 || c > 0x7E) {
            ascii.clear();
            break;
        }
        ascii += c;
    }
    if (!ascii.empty()) {
        return "'" + ascii + "'";
    }

    char buffer[11];
    std::snprintf(buffer, sizeof(buffer), "0x%08X", type);
    return buffer;
}

} // namespace ts2::types
