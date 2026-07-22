#pragma once
/// tileset_hollow — hollow out LOD levels in 3D Tiles 1.1 tileset JSON
///
/// Pre-order traversal: removes content URI for specified LOD levels,
/// geometricError is inherited top-down from the nearest non-hollowed ancestor.

#include <nlohmann/json.hpp>
#include <regex>
#include <sstream>
#include <string>
#include <unordered_set>

// ---------------------------------------------------------------------------
// Extract LOD level from content.uri
// ---------------------------------------------------------------------------
// PagedLOD:  ".../Tile_-001_+050_L22_00000000.glb" -> regex _L(\d+)_     -> 22
// HLOD:      ".../L3_X-050_Y+050.glb"              -> regex (^|/)L(\d+)_ -> 3
// No level:  ".../root.glb" / ".../Tile_-001_+050.glb" -> -1
inline int extract_level(const std::string& uri)
{
    // PagedLOD pattern: _L<digits>_
    static const std::regex re_paged(R"(_L(\d+)_)");
    std::smatch m;
    if (std::regex_search(uri, m, re_paged)) {
        return std::stoi(m[1].str());
    }

    // HLOD pattern: basename starts with L<digits>_
    auto pos = uri.find_last_of("/\\");
    std::string basename = (pos == std::string::npos) ? uri : uri.substr(pos + 1);
    static const std::regex re_hlod(R"(^L(\d+)_)");
    if (std::regex_search(basename, m, re_hlod)) {
        return std::stoi(m[1].str());
    }

    return -1;
}

// ---------------------------------------------------------------------------
// Pre-order recursive hollowing
// ---------------------------------------------------------------------------
// effective_ge: geometricError of the nearest non-hollowed ancestor (top-down)
inline void process_node(nlohmann::json& node,
                         const std::unordered_set<int>& hollow_set,
                         double effective_ge)
{
    // ---- 1. Determine this node's LOD level ----
    int level = -1;
    if (node.contains("content") && node["content"].is_object() &&
        node["content"].contains("uri")) {
        level = extract_level(node["content"]["uri"].get<std::string>());
    }

    bool should_hollow = (level >= 0 && hollow_set.count(level) > 0);

    // ---- 2. Update effective_ge from this node (if real) ----
    if (node.contains("geometricError") && !should_hollow) {
        effective_ge = node["geometricError"].get<double>();
    }

    // ---- 3. Hollow if needed ----
    if (should_hollow) {
        node.erase("content");
        node["geometricError"] = effective_ge;
        // effective_ge continues downward unchanged
    }

    // ---- 4. Recurse into children ----
    if (node.contains("children") && node["children"].is_array()) {
        for (auto& child : node["children"]) {
            process_node(child, hollow_set, effective_ge);
        }
    }
}

// ---------------------------------------------------------------------------
// Parse --levels argument -> std::unordered_set<int>
// ---------------------------------------------------------------------------
// Supports: "15" | "15,16,17" | "15-21" | "15-18,20,22"
inline std::unordered_set<int> parse_levels(const std::string& arg)
{
    std::unordered_set<int> result;
    std::stringstream ss(arg);
    std::string part;

    while (std::getline(ss, part, ',')) {
        auto dash = part.find('-');
        if (dash != std::string::npos) {
            int from = std::stoi(part.substr(0, dash));
            int to   = std::stoi(part.substr(dash + 1));
            if (from > to) std::swap(from, to);
            for (int i = from; i <= to; ++i) result.insert(i);
        } else {
            result.insert(std::stoi(part));
        }
    }
    return result;
}
