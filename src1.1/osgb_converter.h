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
    bool enable_gpu_texture_compress = false; // BasisU ETC1S OpenCL acceleration
    bool gpu_texture_serialize = false;       // serialize OpenCL queues for unstable drivers

    // Geoid
    std::string geoid_model = "none";
    std::string geoid_path;

    // Tileset JSON splitting (external tilesets)
    bool enable_split_json = false;    // split monolithic tileset.json into index + sub-tilesets
    int  split_depth = 0;              // 0=auto, >0=fixed HLOD display depth
    int  split_target_tiles = 256;     // auto mode target source tiles per HLOD shard
    int  lod_step = 2;                 // keep every Nth node in single-child LOD chains
    bool enable_fine_merge = true;     // merge small finest-LOD spatial subtrees
    int  fine_merge_max_sources = 16;  // max leaf OSGB files in one fine aggregate
    int  fine_merge_max_input_mb = 64; // max total source bytes in one fine aggregate

    // Root tile reconstruction (merge coarsest LODs into overview GLB)
    bool enable_top_reconstruct = false;
    int  top_texture_max_size = 512;  // max texture dimension for root GLB (0=no limit)
    int  hlod_max_source_tiles = 16;  // max source tiles merged into one HLOD GLB (0=unlimited)
    int  hlod_max_output_mb = 8;      // omit HLOD drawables above this size (0=unlimited)
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
