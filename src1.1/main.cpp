/**
 * osgb2b3dm - OSGB to 3D Tiles Converter
 *
 * Converts OpenSceneGraph Binary (OSGB) format tilesets to
 * Cesium 3D Tiles 1.1 (GLB + tileset.json) format.
 * Uses 3DTILES_content_gltf extension for raw glTF content.
 *
 * Based on the 3dtiles project (https://github.com/fanvanzh/3dtiles)
 *
 * Usage:
 *   osgb_converter -i <input_dir> -o <output_dir> [options]
 *
 * Or set parameters directly in the HARDCODED_CONFIG section below.
 */

#include "osgb_converter.h"
#include "geoid_height.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <cpl_conv.h>   // CPLSetConfigOption
#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#endif
namespace fs = std::filesystem;

// ============================================================
// 硬编码配置 —— 直接在这里设置参数，无需命令行传参
// 设置 USE_HARDCODED_CONFIG 为 1 启用，设为 0 则走命令行
// ============================================================
#define USE_HARDCODED_CONFIG  0

struct HardcodedConfig {
    const char* input_dir;
    const char* output_dir;
    const char* config_json;
    const char* geoid_model;
    const char* geoid_path;
    bool enable_texture_compress;
    bool enable_meshopt;
    bool enable_draco;
    bool enable_unlit;
    bool enable_top_reconstruct;
    int  top_texture_max_size;
    double simplify_ratio;
    int  draco_pos_bits;
    int  draco_normal_bits;
    int  draco_uv_bits;
    int  ktx2_quality;
    bool enable_parallel;
    int  num_threads;
    bool enable_split_json;
    int  split_depth;
    double override_lon;
    double override_lat;
    double override_alt;
    bool   has_override_alt;
};

static const HardcodedConfig g_config = {
    /* input_dir   */ R"(E:\learning\data\1)",
    /* output_dir  */ R"(E:\learning\data\output\OSG_CJIAJIAbase3dtiles_1_1_no_compresion)",
    /* config_json */ "",
    /* geoid_model */ "none",
    /* geoid_path  */ "",
    /* texture_compress */ true,
    /* meshopt      */ true,
    /* draco        */ true,
    /* unlit        */ true,
    /* top_reconstruct */ false,
    /* top_texture_max_size */ 512,
    /* simplify_ratio */ 0.5,
    /* draco_pos_bits */ 11,
    /* draco_normal_bits */ 10,
    /* draco_uv_bits */ 12,
    /* ktx2_quality */ 128,
    /* enable_parallel */ true,
    /* num_threads     */ 0,
    /* enable_split_json */ false,
    /* split_depth     */ 1,
    /* override_lon */ 0.0,
    /* override_lat */ 0.0,
    /* override_alt */ 0.0,
    /* has_override_alt */ false,
};

// ============================================================
// OSG/GDAL environment setup
// ============================================================
static bool path_exists(const std::string& path) {
    std::error_code ec;
    return !path.empty() && fs::exists(path, ec);
}

static bool osg_library_path_exists(const std::string& paths) {
#ifdef _WIN32
    const char delimiter = ';';
#else
    const char delimiter = ':';
#endif
    size_t start = 0;
    while (start <= paths.size()) {
        const size_t end = paths.find(delimiter, start);
        const std::string path = paths.substr(
            start, end == std::string::npos ? std::string::npos : end - start);
        if (path_exists(path)) return true;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return false;
}

static void setup_environment(const char* exe_path) {
    std::error_code ec;
    const fs::path absolute_exe = fs::absolute(fs::path(exe_path), ec);
    const std::string exe_dir = ec
        ? fs::path(exe_path).parent_path().string()
        : absolute_exe.parent_path().string();

    // Set OSG_LIBRARY_PATH for runtime plugin loading
    // OSG needs to find plugins like osgdb_osg.dll to read .osgb files
    {
        std::string osg_plugins;

        // 1) Keep an existing, valid setting from the launch environment.
        const char* existing_osg = getenv("OSG_LIBRARY_PATH");
        if (existing_osg && osg_library_path_exists(existing_osg)) {
            osg_plugins = existing_osg;
        }

        // 2) Try the platform's vcpkg plugins directory.
        const char* vcpkg_root = getenv("VCPKG_ROOT");
        if (osg_plugins.empty() && vcpkg_root) {
#ifdef _WIN32
            const char* default_triplet = "x64-windows";
#else
            const char* default_triplet = "x64-linux";
#endif
            const char* configured_triplet = getenv("VCPKG_DEFAULT_TRIPLET");
            const std::string triplet =
                configured_triplet && configured_triplet[0]
                    ? configured_triplet : default_triplet;
            const std::string installed =
                (fs::path(vcpkg_root) / "installed" / triplet).string();
            const std::string release_plugins =
                (fs::path(installed) / "plugins" / "osgPlugins-3.6.5").string();
            const std::string debug_plugins =
                (fs::path(installed) / "debug" / "plugins" / "osgPlugins-3.6.5").string();
            if (path_exists(release_plugins)) osg_plugins = release_plugins;
            else if (path_exists(debug_plugins)) osg_plugins = debug_plugins;
        }

        // 3) Fall back to a local directory next to the executable.
        if (osg_plugins.empty()) {
#ifdef _WIN32
            const std::string versioned_plugins =
                (fs::path(exe_dir) / "osgPlugins-3.6.5").string();
            const std::string packaged_plugins =
                (fs::path(exe_dir) / "osgPlugins").string();
#else
            const std::string packaged_plugins =
                (fs::path(exe_dir) / "osgPlugins").string();
            const std::string versioned_plugins =
                (fs::path(exe_dir) / "osgPlugins-3.6.5").string();
#endif
            if (path_exists(versioned_plugins)) osg_plugins = versioned_plugins;
            else if (path_exists(packaged_plugins)) osg_plugins = packaged_plugins;
        }

        if (!osg_plugins.empty()) {
            std::string env_str = "OSG_LIBRARY_PATH=" + osg_plugins;
#ifdef _WIN32
            _putenv(env_str.c_str());
#else
            setenv("OSG_LIBRARY_PATH", osg_plugins.c_str(), 1);
#endif
            fprintf(stderr, "OSG_LIBRARY_PATH set to: %s\n", osg_plugins.c_str());
        } else {
            fprintf(stderr, "WARNING: OSG_LIBRARY_PATH not set — OSG plugins may not load!\n");
        }
    }

    // Find GDAL_DATA and PROJ_LIB.
    // Need proj.db (PROJ) and epsg.wkt (GDAL) for coordinate transformations
    std::string gdal_data_path;
    std::string proj_lib_path;

    // 1) Keep valid settings supplied by the launch environment.
    const char* existing_gdal = getenv("GDAL_DATA");
    if (existing_gdal && path_exists((fs::path(existing_gdal) / "epsg.wkt").string())) {
        gdal_data_path = existing_gdal;
    }
    const char* existing_proj = getenv("PROJ_LIB");
    if (existing_proj && path_exists((fs::path(existing_proj) / "proj.db").string())) {
        proj_lib_path = existing_proj;
    }

    // 2) Check the platform's vcpkg installation.
    const char* vcpkg_root = getenv("VCPKG_ROOT");
    if ((gdal_data_path.empty() || proj_lib_path.empty()) && vcpkg_root) {
#ifdef _WIN32
        const char* default_triplet = "x64-windows";
#else
        const char* default_triplet = "x64-linux";
#endif
        const char* configured_triplet = getenv("VCPKG_DEFAULT_TRIPLET");
        const std::string triplet =
            configured_triplet && configured_triplet[0]
                ? configured_triplet : default_triplet;
        const fs::path vcpkg_share =
            fs::path(vcpkg_root) / "installed" / triplet / "share";
        const std::string vcpkg_gdal = (vcpkg_share / "gdal").string();
        const std::string vcpkg_proj = (vcpkg_share / "proj").string();
        if (gdal_data_path.empty()
            && path_exists((fs::path(vcpkg_gdal) / "epsg.wkt").string())) {
            gdal_data_path = vcpkg_gdal;
        }
        if (proj_lib_path.empty()
            && path_exists((fs::path(vcpkg_proj) / "proj.db").string())) {
            proj_lib_path = vcpkg_proj;
        }
    }

    // 3) Fall back to directories next to the executable.
    if (gdal_data_path.empty()) {
#ifdef _WIN32
        const std::string primary_gdal = (fs::path(exe_dir) / "gdal").string();
        const std::string packaged_gdal = (fs::path(exe_dir) / "share" / "gdal").string();
#else
        const std::string primary_gdal = (fs::path(exe_dir) / "share" / "gdal").string();
        const std::string packaged_gdal = (fs::path(exe_dir) / "gdal").string();
#endif
        if (path_exists((fs::path(primary_gdal) / "epsg.wkt").string()))
            gdal_data_path = primary_gdal;
        else if (path_exists((fs::path(packaged_gdal) / "epsg.wkt").string()))
            gdal_data_path = packaged_gdal;
    }
    if (proj_lib_path.empty()) {
#ifdef _WIN32
        const std::string primary_proj = (fs::path(exe_dir) / "proj").string();
        const std::string packaged_proj = (fs::path(exe_dir) / "share" / "proj").string();
#else
        const std::string primary_proj = (fs::path(exe_dir) / "share" / "proj").string();
        const std::string packaged_proj = (fs::path(exe_dir) / "proj").string();
#endif
        if (path_exists((fs::path(primary_proj) / "proj.db").string()))
            proj_lib_path = primary_proj;
        else if (path_exists((fs::path(packaged_proj) / "proj.db").string()))
            proj_lib_path = packaged_proj;
    }

    // Only set valid paths; never erase settings supplied by the caller.
    if (!gdal_data_path.empty()) {
        std::string gdal_env = "GDAL_DATA=" + gdal_data_path;
#ifdef _WIN32
        _putenv(gdal_env.c_str());
#else
        setenv("GDAL_DATA", gdal_data_path.c_str(), 1);
#endif
        CPLSetConfigOption("GDAL_DATA", gdal_data_path.c_str());
        fprintf(stderr, "GDAL_DATA set to: %s\n", gdal_data_path.c_str());
    } else {
        fprintf(stderr, "WARNING: GDAL_DATA not found! Coordinate transforms may fail.\n");
    }

    if (!proj_lib_path.empty()) {
        std::string proj_env = "PROJ_LIB=" + proj_lib_path;
#ifdef _WIN32
        _putenv(proj_env.c_str());
#else
        setenv("PROJ_LIB", proj_lib_path.c_str(), 1);
#endif
        CPLSetConfigOption("PROJ_LIB", proj_lib_path.c_str());
        fprintf(stderr, "PROJ_LIB set to: %s\n", proj_lib_path.c_str());
    } else {
        fprintf(stderr, "WARNING: PROJ_LIB not found! Coordinate transforms may fail.\n");
    }
}

// ============================================================
// Print usage
// ============================================================
static void print_usage(const char* prog) {
    fprintf(stderr,
        "osgb2b3dm - OSGB to 3D Tiles Converter\n"
        "Usage: %s -i <input_dir> -o <output_dir> [options]\n"
        "\n"
        "Required:\n"
        "  -i, --input  <dir>      Input OSGB tileset directory\n"
        "  -o, --output <dir>      Output 3D Tiles directory\n"
        "\n"
        "Optional:\n"
        "  -c, --config <json>     Tile config JSON: {\"x\":lon,\"y\":lat,\"max_lvl\":20}\n"
        "  --enable-draco          Enable Draco mesh compression\n"
        "  --enable-simplify       Enable mesh simplification\n"
        "  --enable-texture-compress  Enable KTX2 texture compression\n"
        "  --enable-lod            Enable Level of Detail\n"
        "  --enable-unlit          Enable KHR_materials_unlit (default: on for OSGB)\n"
        "  --enable-top-reconstruct  Merge coarsest LOD tiles into a root overview GLB\n"
        "  --top-texture-max-size N  Max texture dim for root GLB (default: 512, 0=no limit)\n"
        "  --simplify-ratio R      Meshopt target_ratio (default: 0.5, 1.0=no simplify)\n"
        "  --draco-pos-bits N      Draco position quant bits (default: 11)\n"
        "  --draco-normal-bits N   Draco normal quant bits (default: 10)\n"
        "  --draco-uv-bits N       Draco UV quant bits (default: 12)\n"
        "  --ktx2-quality N      KTX2 encode quality (1-255, lower=faster, default: 128)\n"
        "  --no-parallel           Disable all multi-threading (Phase 1 + Phase 2)\n"
        "  --threads N             Number of worker threads (default: auto=CPU cores)\n"
        "  --split-json            Split tileset.json into root index + sub-tilesets\n"
        "  --split-depth N         Split depth (default: 1, 1=per top-level tile)\n"
        "  --geoid <model>         Geoid model: none, egm84, egm96, egm2008\n"
        "  --geoid-path <path>     Path to geoid data files\n"
        "  --lon <degrees>         Override longitude\n"
        "  --lat <degrees>         Override latitude\n"
        "  --alt <meters>          Override altitude\n"
        "  -h, --help              Show this help\n"
        "\n"
        "Example:\n"
        "  %s -i ./tileset_data -o ./output_3dtiles -c '{\"x\":120.0,\"y\":30.0,\"max_lvl\":20}'\n",
        prog, prog);
}

// ============================================================
// Main
// ============================================================
int main(int argc, char* argv[]) {
    // Setup environment
    setup_environment(argv[0]);

    // Parse arguments
    osgb_converter::ConvertOptions opts;

#if USE_HARDCODED_CONFIG
    // -------- 直接使用硬编码配置 --------
    opts.input_dir  = g_config.input_dir;
    opts.output_dir = g_config.output_dir;
    opts.config_json = g_config.config_json;

    opts.geoid_model = g_config.geoid_model;
    opts.geoid_path  = g_config.geoid_path;

    opts.enable_texture_compress = g_config.enable_texture_compress;
    opts.enable_meshopt          = g_config.enable_meshopt;
    opts.enable_draco            = g_config.enable_draco;
    opts.enable_unlit            = g_config.enable_unlit;
    opts.enable_top_reconstruct  = g_config.enable_top_reconstruct;
    opts.top_texture_max_size    = g_config.top_texture_max_size;
    opts.simplify_ratio          = g_config.simplify_ratio;
    opts.draco_pos_bits          = g_config.draco_pos_bits;
    opts.draco_normal_bits       = g_config.draco_normal_bits;
    opts.draco_uv_bits           = g_config.draco_uv_bits;
    opts.ktx2_quality            = g_config.ktx2_quality;
    opts.enable_parallel         = g_config.enable_parallel;
    opts.num_threads             = g_config.num_threads;
    opts.enable_split_json       = g_config.enable_split_json;
    opts.split_depth             = g_config.split_depth;

    if (g_config.override_lon != 0.0) opts.center_x = g_config.override_lon;
    if (g_config.override_lat != 0.0) opts.center_y = g_config.override_lat;
    if (g_config.has_override_alt) {
        opts.region_offset     = g_config.override_alt;
        opts.has_region_offset = true;
    }

    fprintf(stderr, "[config] Using hardcoded config:\n");
    fprintf(stderr, "  input:  %s\n", opts.input_dir.c_str());
    fprintf(stderr, "  output: %s\n", opts.output_dir.c_str());
    fprintf(stderr, "  geoid:  %s\n", opts.geoid_model.c_str());
    // -------- 跳过 CLI 参数解析 --------
#else
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];

        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if ((arg == "-i" || arg == "--input") && i + 1 < argc) {
            opts.input_dir = argv[++i];
        } else if ((arg == "-o" || arg == "--output") && i + 1 < argc) {
            opts.output_dir = argv[++i];
        } else if ((arg == "-c" || arg == "--config") && i + 1 < argc) {
            opts.config_json = argv[++i];
        } else if (arg == "--enable-draco") {
            opts.enable_draco = true;
        } else if (arg == "--enable-simplify") {
            opts.enable_meshopt = true;
        } else if (arg == "--enable-texture-compress") {
            opts.enable_texture_compress = true;
        } else if (arg == "--enable-lod") {
            LOG_I("LOD enabled (not fully implemented)");
        } else if (arg == "--enable-unlit") {
            opts.enable_unlit = true;
        } else if (arg == "--no-parallel") {
            opts.enable_parallel = false;
        } else if (arg == "--threads" && i + 1 < argc) {
            opts.num_threads = std::stoi(argv[++i]);
        } else if (arg == "--split-json") {
            opts.enable_split_json = true;
        } else if (arg == "--split-depth" && i + 1 < argc) {
            opts.split_depth = std::stoi(argv[++i]);
        } else if (arg == "--enable-top-reconstruct" || arg == "--enable-top_reconstruct") {
            opts.enable_top_reconstruct = true;
        } else if (arg == "--top-texture-max-size" && i + 1 < argc) {
            opts.top_texture_max_size = std::stoi(argv[++i]);
        } else if (arg == "--simplify-ratio" && i + 1 < argc) {
            opts.simplify_ratio = std::stod(argv[++i]);
        } else if (arg == "--draco-pos-bits" && i + 1 < argc) {
            opts.draco_pos_bits = std::stoi(argv[++i]);
        } else if (arg == "--draco-normal-bits" && i + 1 < argc) {
            opts.draco_normal_bits = std::stoi(argv[++i]);
        } else if (arg == "--draco-uv-bits" && i + 1 < argc) {
            opts.draco_uv_bits = std::stoi(argv[++i]);
        } else if (arg == "--ktx2-quality" && i + 1 < argc) {
            opts.ktx2_quality = std::stoi(argv[++i]);
        } else if (arg == "--geoid" && i + 1 < argc) {
            opts.geoid_model = argv[++i];
        } else if (arg == "--geoid-path" && i + 1 < argc) {
            opts.geoid_path = argv[++i];
        } else if (arg == "--lon" && i + 1 < argc) {
            opts.center_x = std::stod(argv[++i]);
        } else if (arg == "--lat" && i + 1 < argc) {
            opts.center_y = std::stod(argv[++i]);
        } else if (arg == "--alt" && i + 1 < argc) {
            opts.region_offset = std::stod(argv[++i]);
            opts.has_region_offset = true;
        } else {
            fprintf(stderr, "Unknown option: %s\n", arg.c_str());
            print_usage(argv[0]);
            return 1;
        }
    }
#endif  // USE_HARDCODED_CONFIG

    // Validate required args
    if (opts.input_dir.empty() || opts.output_dir.empty()) {
        fprintf(stderr, "Error: --input and --output are required.\n\n");
        print_usage(argv[0]);
        return 1;
    }

    // Initialize geoid if requested
    if (opts.geoid_model != "none" && !opts.geoid_model.empty()) {
        fprintf(stderr, "Initializing geoid model: %s\n", opts.geoid_model.c_str());
        auto model = GeoidHeight::GeoidCalculator::StringToGeoidModel(opts.geoid_model);
        if (!GeoidHeight::InitializeGlobalGeoidCalculator(model, opts.geoid_path)) {
            fprintf(stderr, "Warning: Failed to initialize geoid model %s\n", opts.geoid_model.c_str());
        }
    }

    // Run conversion
    int result = osgb_converter::convert_osgb(opts);

    // Cleanup
    GeoidHeight::GetGlobalGeoidCalculator();

    return result;
}
