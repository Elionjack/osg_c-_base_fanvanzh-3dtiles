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
    bool enable_parallel = true;     // multi-threaded tile conversion
    int  num_threads = 0;            // thread count (0=auto: hardware_concurrency)
    int  ktx2_quality = 128;         // basisu encoding quality (lower=faster, 1-255)

    // Geoid
    std::string geoid_model = "none";
    std::string geoid_path;

    // Tileset JSON splitting (external tilesets)
    bool enable_split_json = false;    // split monolithic tileset.json into index + sub-tilesets
    int  split_depth = 1;              // split depth (1 = one sub-tileset per top-level tree)

    // Root tile reconstruction (merge coarsest LODs into overview GLB)
    bool enable_top_reconstruct = false;
    int  top_texture_max_size = 512;  // max texture dimension for root GLB (0=no limit)
    double simplify_ratio = 0.5;       // meshopt target_ratio (1.0=no simplify)
    int  draco_pos_bits = 11;          // Draco position quantization bits
    int  draco_normal_bits = 10;       // Draco normal quantization bits
    int  draco_uv_bits = 12;           // Draco texcoord quantization bits

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
