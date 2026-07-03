#pragma once

#include <string>
#include <optional>
#include "utils.h"

namespace osgb_converter {

struct ConvertOptions {
    std::string input_dir;
    std::string output_dir;
    std::string config_json;       // JSON config: {"x":lon, "y":lat, "offset":h, "max_lvl":N, "pbr":bool}
    int max_lvl = 100;

    // Feature flags
    bool enable_texture_compress = false;
    bool enable_meshopt = false;
    bool enable_draco = false;
    bool enable_unlit = true;

    // Geoid
    std::string geoid_model = "none";
    std::string geoid_path;

    // Override values from config/cli
    double center_x = 0.0;   // longitude
    double center_y = 0.0;   // latitude
    double region_offset = 0.0;
    bool has_region_offset = false;
};

// Main entry point: convert an OSGB tileset directory to 3D Tiles
// Returns 0 on success, non-zero on failure.
int convert_osgb(const ConvertOptions& opts);

} // namespace osgb_converter
