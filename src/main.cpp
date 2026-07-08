/**
 * osgb2b3dm - OSGB to 3D Tiles Converter
 *
 * Converts OpenSceneGraph Binary (OSGB) format tilesets to
 * Cesium 3D Tiles (B3DM + tileset.json) format.
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
    double override_lon;
    double override_lat;
    double override_alt;
    bool   has_override_alt;
};

static const HardcodedConfig g_config = {
    /* input_dir   */ R"(E:\learning\data\1)",
    /* output_dir  */ R"(E:\learning\data\output\OSG_CJIAJIAbase3dtiles)",
    /* config_json */ "",
    /* geoid_model */ "none",
    /* geoid_path  */ "",
    /* texture_compress */ false,
    /* meshopt      */ false,
    /* draco        */ false,
    /* unlit        */ true,
    /* override_lon */ 0.0,
    /* override_lat */ 0.0,
    /* override_alt */ 0.0,
    /* has_override_alt */ false,
};

// ============================================================
// OSG/GDAL environment setup
// ============================================================
static void setup_environment(const char* exe_path) {
    // Derive executable directory
    std::string exe_dir;
    {
        std::string ep(exe_path);
        auto pos = ep.find_last_of("/\\");
        if (pos != std::string::npos) exe_dir = ep.substr(0, pos);
    }

    // Set OSG_LIBRARY_PATH for runtime plugin loading
    // OSG needs to find plugins like osgdb_osg.dll to read .osgb files
    {
        std::string osg_plugins;

        // 1) Try vcpkg debug plugins directory first
        const char* vcpkg_root = getenv("VCPKG_ROOT");
        if (vcpkg_root) {
            std::string vcpkg_debug_plugins = std::string(vcpkg_root)
                + "/installed/x64-windows/debug/plugins/osgPlugins-3.6.5";
            std::string vcpkg_rel_plugins  = std::string(vcpkg_root)
                + "/installed/x64-windows/plugins/osgPlugins-3.6.5";
#ifdef _WIN32
            if (GetFileAttributesA(vcpkg_debug_plugins.c_str()) != INVALID_FILE_ATTRIBUTES) {
                osg_plugins = vcpkg_debug_plugins;
            } else if (GetFileAttributesA(vcpkg_rel_plugins.c_str()) != INVALID_FILE_ATTRIBUTES) {
                osg_plugins = vcpkg_rel_plugins;
            }
#else
            if (fs::exists(vcpkg_debug_plugins)) osg_plugins = vcpkg_debug_plugins;
            else if (fs::exists(vcpkg_rel_plugins)) osg_plugins = vcpkg_rel_plugins;
#endif
        }

        // 2) Fallback: local directory next to executable
        if (osg_plugins.empty()) {
            std::string local_plugins = exe_dir + "/osgPlugins-3.6.5";
#ifdef _WIN32
            if (GetFileAttributesA(local_plugins.c_str()) != INVALID_FILE_ATTRIBUTES)
                osg_plugins = local_plugins;
#else
            if (fs::exists(local_plugins)) osg_plugins = local_plugins;
#endif
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

    // Find GDAL_DATA and PROJ_LIB — try vcpkg first, then local dirs
    // Need proj.db (PROJ) and epsg.wkt (GDAL) for coordinate transformations
    std::string gdal_data_path;
    std::string proj_lib_path;

    // 1) Check VCPKG_ROOT environment variable
    {
        const char* vcpkg_root = getenv("VCPKG_ROOT");
        if (vcpkg_root) {
            std::string vcpkg_share = std::string(vcpkg_root) + "/installed/x64-windows/share";
            std::string vcpkg_gdal = vcpkg_share + "/gdal";
            std::string vcpkg_proj = vcpkg_share + "/proj";
#ifdef _WIN32
            if (GetFileAttributesA((vcpkg_proj + "/proj.db").c_str()) != INVALID_FILE_ATTRIBUTES
                && GetFileAttributesA((vcpkg_gdal + "/epsg.wkt").c_str()) != INVALID_FILE_ATTRIBUTES) {
                gdal_data_path = vcpkg_gdal;
                proj_lib_path = vcpkg_proj;
            }
#else
            if (fs::exists(vcpkg_proj + "/proj.db") && fs::exists(vcpkg_gdal + "/epsg.wkt")) {
                gdal_data_path = vcpkg_gdal;
                proj_lib_path = vcpkg_proj;
            }
#endif
        }
    }

    // 2) Fallback: local directories next to executable
    if (gdal_data_path.empty()) {
        std::string gdal_local = exe_dir + "/gdal";
#ifdef _WIN32
        if (GetFileAttributesA((gdal_local + "/epsg.wkt").c_str()) != INVALID_FILE_ATTRIBUTES)
            gdal_data_path = gdal_local;
#else
        if (fs::exists(gdal_local + "/epsg.wkt"))
            gdal_data_path = gdal_local;
#endif
    }
    if (proj_lib_path.empty()) {
        std::string proj_local = exe_dir + "/proj";
#ifdef _WIN32
        if (GetFileAttributesA((proj_local + "/proj.db").c_str()) != INVALID_FILE_ATTRIBUTES)
            proj_lib_path = proj_local;
#else
        if (fs::exists(proj_local + "/proj.db"))
            proj_lib_path = proj_local;
#endif
    }

    // Set GDAL_DATA
    // IMPORTANT: On Windows, SetEnvironmentVariableA doesn't update CRT's _environ,
    // so getenv() still returns the old value. Must use _putenv() + CPLSetConfigOption.
    {
        std::string gdal_env = "GDAL_DATA=" + gdal_data_path;
#ifdef _WIN32
        _putenv(gdal_env.c_str());
#else
        setenv("GDAL_DATA", gdal_data_path.c_str(), 1);
#endif
        CPLSetConfigOption("GDAL_DATA", gdal_data_path.c_str());
        fprintf(stderr, "GDAL_DATA set to: %s\n", gdal_data_path.c_str());
    }

    // Set PROJ_LIB — critical for WKT SRS parsing
    {
        std::string proj_env = "PROJ_LIB=" + proj_lib_path;
#ifdef _WIN32
        _putenv(proj_env.c_str());
#else
        setenv("PROJ_LIB", proj_lib_path.c_str(), 1);
#endif
        CPLSetConfigOption("PROJ_LIB", proj_lib_path.c_str());
        fprintf(stderr, "PROJ_LIB set to: %s\n", proj_lib_path.c_str());
    }

    if (gdal_data_path.empty() || proj_lib_path.empty()) {
        fprintf(stderr, "WARNING: GDAL_DATA or PROJ_LIB not found! "
                "Coordinate transforms will fail.\n");
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
