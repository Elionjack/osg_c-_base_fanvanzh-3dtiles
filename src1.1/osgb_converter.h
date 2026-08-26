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
    bool hlod_only = false;           // generate only HLOD GLBs and an HLOD-only tileset.json
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
    bool skip_bad_tiles = false;       // skip malformed OSGB nodes/grids and write failed_tiles.txt
    int tile_read_timeout = 0;         // seconds per top-level grid (0=legacy thread mode)
    int tile_reader_processes = 4;     // persistent Linux Phase 1 reader processes

    // Finest-LOD subtree aggregation
    bool enable_fine_merge = true;     // merge small finest-LOD spatial subtrees
    int  fine_merge_max_sources = 16;  // max leaf OSGB files in one aggregate
    int  fine_merge_max_input_mb = 64; // max total source bytes in one aggregate

    // Progressive top reconstruction: spatial HLOD depths consume matching
    // source frontiers (coarsest, next-coarsest, next-next-coarsest, ...).
    bool enable_top_reconstruct = false;
    bool use_git_head_top_reconstruct = false; // use the Git HEAD child-intermediate HLOD algorithm
    // Non-Git mode: skip this many numbered source LOD frontiers after Root
    // before building the first HLOD. Git HEAD mode: shift HLOD build-quality
    // settings by this many levels (legacy behavior).
    int  git_head_top_reconstruct_level_offset = 0;
    int  top_texture_max_size = 512;  // max texture dimension for root GLB (0=no limit)
    int  hlod_branching_factor = 16; // spatial children per HLOD node (perfect square, e.g. 4 or 16)
    int  hlod_max_source_tiles = 16;  // compatibility option; progressive HLOD keeps all sources
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

// Internal Linux worker entry point used by the persistent Phase 1 process pool.
// It is intentionally not part of the public command-line interface.
int run_phase1_reader_worker();

} // namespace osgb_converter
