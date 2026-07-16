#include "osgb_converter.h"
#include "osg_gltf_converter.h"
#include "coordinate_system.h"
#include "coordinate_transformer.h"
#include "geoid_height.h"

#include <nlohmann/json.hpp>
#include <ogr_spatialref.h>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <future>
#include <functional>
#include <memory>
#include <glm/gtc/type_ptr.hpp>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace osgb_converter {

// Forward declaration
static std::vector<std::string> split_string(const std::string& s, char delim);

// ============================================================
// metadata.xml parsing
// ============================================================
struct ModelMetadata {
    std::string version;
    std::string SRS;
    std::string SRSOrigin;
};

static bool parse_metadata_xml(const std::string& dir, ModelMetadata& meta) {
    fs::path meta_file = fs::path(dir) / "metadata.xml";
    if (!fs::exists(meta_file)) {
        LOG_E("metadata.xml not found at %s", meta_file.string().c_str());
        return false;
    }

    std::ifstream ifs(meta_file);
    if (!ifs.is_open()) {
        LOG_E("cannot open %s", meta_file.string().c_str());
        return false;
    }

    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    ifs.close();
    LOG_I("metadata.xml content: %s", content.c_str());

    // Simple XML parsing for SRS and SRSOrigin (manual parsing to avoid XML dependency)
    auto extract_tag = [&](const std::string& tag) -> std::string {
        std::string open = "<" + tag + ">";
        std::string close = "</" + tag + ">";
        auto start = content.find(open);
        if (start == std::string::npos) return "";
        start += open.size();
        auto end = content.find(close, start);
        if (end == std::string::npos) return "";
        return content.substr(start, end - start);
    };

    // Also try self-closing / attribute format
    auto extract_attr = [&](const std::string& tag, const std::string& attr) -> std::string {
        std::string search = "<" + tag + " ";
        auto start = content.find(search);
        if (start == std::string::npos) {
            search = "<" + tag + ">";
            start = content.find(search);
            if (start == std::string::npos) return "";
        }
        auto attrPos = content.find(attr + "=\"", start);
        if (attrPos == std::string::npos || attrPos > content.find(">", start)) return "";
        attrPos += attr.size() + 2;
        auto end = content.find("\"", attrPos);
        if (end == std::string::npos) return "";
        return content.substr(attrPos, end - attrPos);
    };

    meta.SRS = extract_tag("SRS");
    if (meta.SRS.empty()) meta.SRS = extract_attr("ModelMetadata", "SRS");
    meta.SRSOrigin = extract_tag("SRSOrigin");
    if (meta.SRSOrigin.empty()) meta.SRSOrigin = extract_attr("ModelMetadata", "SRSOrigin");

    // Decode XML entities (&quot; → ", &amp; → &, etc.)
    // Critical for WKT SRS which uses quotes around projection names
    auto xml_decode = [](std::string& s) {
        size_t pos;
        while ((pos = s.find("&quot;")) != std::string::npos)
            s.replace(pos, 6, "\"");
        while ((pos = s.find("&amp;")) != std::string::npos)
            s.replace(pos, 5, "&");
        while ((pos = s.find("&lt;")) != std::string::npos)
            s.replace(pos, 4, "<");
        while ((pos = s.find("&gt;")) != std::string::npos)
            s.replace(pos, 4, ">");
    };
    xml_decode(meta.SRS);
    xml_decode(meta.SRSOrigin);

    LOG_I("Parsed metadata: SRS=%s, SRSOrigin=%s", meta.SRS.c_str(), meta.SRSOrigin.c_str());
    return !meta.SRS.empty();
}

// ============================================================
// Coordinate transformer initialization
// ============================================================
static bool init_coordinate_transformer(const ModelMetadata& meta,
                                        double& center_x, double& center_y,
                                        std::optional<std::tuple<double,double,double>>& enu_offset,
                                        std::optional<double>& origin_height,
                                        const std::string& gdal_data, const std::string& proj_lib) {
    // Set GDAL/PROJ paths
    if (!gdal_data.empty()) CPLSetConfigOption("GDAL_DATA", gdal_data.c_str());
    if (!proj_lib.empty()) CPLSetConfigOption("PROJ_LIB", proj_lib.c_str());

    // Verify proj.db accessibility (critical for WKT parsing)
    fprintf(stderr, "[GDAL] GDAL_DATA=%s\n", CPLGetConfigOption("GDAL_DATA", "NOT_SET"));
    fprintf(stderr, "[GDAL] PROJ_LIB=%s\n", CPLGetConfigOption("PROJ_LIB", "NOT_SET"));
    {
        std::string proj_db = proj_lib.empty() ? "" : proj_lib + "/proj.db";
        if (!proj_db.empty() && fs::exists(proj_db)) {
            fprintf(stderr, "[GDAL] proj.db found at: %s\n", proj_db.c_str());
        } else {
            fprintf(stderr, "[GDAL] WARNING: proj.db NOT found at: %s\n", proj_db.c_str());
            fprintf(stderr, "[GDAL] PROJ coordinate transforms will fail!\n");
        }
    }

    // Parse SRS
    std::string srs = meta.SRS;
    auto colon = srs.find(':');
    if (colon == std::string::npos) {
        // Try WKT
        LOG_I("SRS has no colon, treating as WKT");
        std::vector<double> pt = {0, 0, 0};
        auto parts = split_string(meta.SRSOrigin, ',');
        for (size_t i = 0; i < std::min(parts.size(), (size_t)3); i++)
            pt[i] = std::stod(parts[i]);

        auto cs = coords::CoordinateSystem::WKT(srs, pt[0], pt[1], pt[2]);
        auto geo_ref = coords::GeoReference::FromDegrees(0, 0, 0);

        // Create temp OGR transform to get geographic origin
        OGRSpatialReference inRs, outRs;
        inRs.importFromWkt(srs.c_str());
        outRs.importFromEPSG(4326);
        inRs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        outRs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        auto* poCT = OGRCreateCoordinateTransformation(&inRs, &outRs);
        if (poCT) {
            double lon = pt[0], lat = pt[1], h = pt[2];
            fprintf(stderr, "[WKT] Origin in source CRS: x=%.6f y=%.6f z=%.6f\n", lon, lat, h);
            poCT->Transform(1, &lon, &lat, &h);
            center_x = lon; center_y = lat;
            fprintf(stderr, "[WKT] Origin in WGS84: lon=%.10f lat=%.10f h=%.3f\n", lon, lat, h);
            OGRCoordinateTransformation::DestroyCT(poCT);
        } else {
            fprintf(stderr, "[WKT] ERROR: Failed to create OGR transform (SRS parse failed?)\n");
        }
        geo_ref = coords::GeoReference::FromDegrees(center_x, center_y, pt[2]);

        auto* transformer = new coords::CoordinateTransformer(cs, geo_ref);
        SetGlobalTransformer(transformer);
        origin_height = transformer->GeoOriginHeight();
        LOG_I("WKT init: center=(%.10f, %.10f)", center_x, center_y);
        return true;
    }

    std::string srs_type = srs.substr(0, colon);
    std::string srs_val = srs.substr(colon + 1);

    if (srs_type == "ENU") {
        // Parse ENU: "ENU:lat,lon"
        auto parts = split_string(srs_val, ',');
        if (parts.size() < 2) {
            LOG_E("invalid ENU SRS: %s", srs.c_str());
            return false;
        }
        center_y = std::stod(parts[0]); // lat
        center_x = std::stod(parts[1]); // lon

        // Parse SRSOrigin
        auto origin_parts = split_string(meta.SRSOrigin, ',');
        double ox = 0, oy = 0, oz = 0;
        if (origin_parts.size() >= 2) {
            ox = std::stod(origin_parts[0]);
            oy = std::stod(origin_parts[1]);
            if (origin_parts.size() >= 3) oz = std::stod(origin_parts[2]);
        }

        fprintf(stderr, "[SRS] ENU: %.7f, %.7f (offset: %.3f, %.3f, %.3f)\n",
                center_y, center_x, ox, oy, oz);

        auto cs = coords::CoordinateSystem::ENU(center_x, center_y, 0.0, ox, oy, oz);
        auto* transformer = new coords::CoordinateTransformer(cs);

        SetGlobalTransformer(transformer);
        enu_offset = std::make_tuple(ox, oy, oz);
        origin_height = transformer->GeoOriginHeight();

        LOG_I("ENU init: center=(%.10f, %.10f), offset=(%.3f,%.3f,%.3f)",
              center_x, center_y, ox, oy, oz);
        return true;
    }

    if (srs_type == "EPSG") {
        int epsg_code = std::stoi(srs_val);
        auto origin_parts = split_string(meta.SRSOrigin, ',');
        double ox = 0, oy = 0, oz = 0;
        if (origin_parts.size() >= 2) {
            ox = std::stod(origin_parts[0]);
            oy = std::stod(origin_parts[1]);
            if (origin_parts.size() >= 3) oz = std::stod(origin_parts[2]);
        }

        fprintf(stderr, "[SRS] EPSG:%d -> EPSG:4326\n", epsg_code);
        fprintf(stderr, "[Origin] x=%.6f y=%.6f z=%.3f\n", ox, oy, oz);

        auto cs = coords::CoordinateSystem::EPSG(epsg_code, ox, oy, oz);

        // Create OGR transform to get geographic origin
        OGRSpatialReference inRs, outRs;
        inRs.importFromEPSG(epsg_code);
        outRs.importFromEPSG(4326);
        inRs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        outRs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        auto* poCT = OGRCreateCoordinateTransformation(&inRs, &outRs);
        if (!poCT) {
            LOG_E("Failed to create OGR transform for EPSG:%d", epsg_code);
            return false;
        }
        double lon = ox, lat = oy, h = oz;
        poCT->Transform(1, &lon, &lat, &h);
        center_x = lon; center_y = lat;
        OGRCoordinateTransformation::DestroyCT(poCT);

        fprintf(stderr, "[Origin LLA] lon=%.10f lat=%.10f h=%.3f\n", lon, lat, h);

        auto geo_ref = coords::GeoReference::FromDegrees(lon, lat, h);

        // Check geoid
        coords::GeoidConfig gc;
        if (GeoidHeight::GetGlobalGeoidCalculator().IsInitialized()) {
            gc = coords::GeoidConfig::EGM96();
            gc.enabled = true;
        }
        auto* transformer = new coords::CoordinateTransformer(cs, geo_ref, gc);
        SetGlobalTransformer(transformer);
        origin_height = transformer->GeoOriginHeight();

        LOG_I("EPSG init: center=(%.10f, %.10f), origin_height=%.3f", center_x, center_y, *origin_height);
        return true;
    }

    LOG_E("Unknown SRS type: %s", srs_type.c_str());
    return false;
}

// Simple string split helper
static std::vector<std::string> split_string(const std::string& s, char delim) {
    std::vector<std::string> result;
    size_t start = 0, end;
    while ((end = s.find(delim, start)) != std::string::npos) {
        result.push_back(s.substr(start, end - start));
        start = end + 1;
    }
    if (start < s.size()) result.push_back(s.substr(start));
    return result;
}

// ============================================================
// ENU→ECEF transform matrix computation
// ============================================================
static std::vector<double> transfrom_xyz(double lon_deg, double lat_deg, double height_min) {
    glm::dmat4 mat = coords::CoordinateTransformer::CalcEnuToEcefMatrix(lon_deg, lat_deg, height_min);
    std::vector<double> result(16);
    const double* ptr = glm::value_ptr(mat);
    std::memcpy(result.data(), ptr, 16 * sizeof(double));
    return result;
}

static std::vector<double> box_to_tileset_box(const std::vector<double>& box_v) {
    std::vector<double> box_new;
    box_new.push_back((box_v[0] + box_v[3]) / 2.0);
    box_new.push_back((box_v[1] + box_v[4]) / 2.0);
    box_new.push_back((box_v[2] + box_v[5]) / 2.0);
    box_new.push_back(std::abs(box_v[3] - box_v[0]) / 2.0);
    box_new.push_back(0.0);
    box_new.push_back(0.0);
    box_new.push_back(0.0);
    box_new.push_back(std::abs(box_v[4] - box_v[1]) / 2.0);
    box_new.push_back(0.0);
    box_new.push_back(0.0);
    box_new.push_back(0.0);
    box_new.push_back(std::abs(box_v[5] - box_v[2]) / 2.0);
    return box_new;
}

// ============================================================
// Main conversion entry point
// ============================================================
int convert_osgb(const ConvertOptions& opts) {
    using namespace std::chrono;
    auto tick = high_resolution_clock::now();

    // ============================================================
    // 1. Validate input
    // ============================================================
    fs::path in_dir(opts.input_dir);
    if (!fs::exists(in_dir) || !fs::is_directory(in_dir)) {
        LOG_E("Input directory does not exist: %s", opts.input_dir.c_str());
        return 1;
    }

    fs::path out_dir(opts.output_dir);
    fs::create_directories(out_dir);

    // ============================================================
    // 2. Parse metadata.xml
    // ============================================================
    ModelMetadata metadata;
    double center_x = opts.center_x;
    double center_y = opts.center_y;
    std::optional<std::tuple<double,double,double>> enu_offset;
    std::optional<double> origin_height;

    if (!parse_metadata_xml(opts.input_dir, metadata)) {
        LOG_E("Failed to parse metadata.xml");
        return 1;
    }

    // GDAL/PROJ data paths
    std::string gdal_data, proj_lib;
    // Try to find GDAL/PROJ data relative to executable
    const char* gdal_env = getenv("GDAL_DATA");
    const char* proj_env = getenv("PROJ_LIB");
    if (gdal_env) gdal_data = gdal_env;
    if (proj_env) proj_lib = proj_env;

    // Parse JSON config override
    double cfg_x = 0, cfg_y = 0, cfg_offset = 0;
    int cfg_max_lvl = 100;
    bool has_cfg = false;
    if (!opts.config_json.empty()) {
        try {
            json cfg = json::parse(opts.config_json);
            if (cfg.contains("x")) { cfg_x = cfg["x"].get<double>(); has_cfg = true; }
            if (cfg.contains("y")) { cfg_y = cfg["y"].get<double>(); has_cfg = true; }
            if (cfg.contains("offset")) cfg_offset = cfg["offset"].get<double>();
            if (cfg.contains("max_lvl")) cfg_max_lvl = cfg["max_lvl"].get<int>();
        } catch (...) {
            LOG_E("Failed to parse config JSON: %s", opts.config_json.c_str());
        }
    }

    // ============================================================
    // 3. Initialize coordinate transformer
    // ============================================================
    if (!init_coordinate_transformer(metadata, center_x, center_y, enu_offset, origin_height, gdal_data, proj_lib)) {
        LOG_E("Failed to initialize coordinate transformer");
        return 1;
    }

    // Override with config values if provided
    if (has_cfg) {
        center_x = (opts.center_x != 0) ? opts.center_x : cfg_x;
        center_y = (opts.center_y != 0) ? opts.center_y : cfg_y;
    }

    int max_lvl = (opts.max_lvl != 100) ? opts.max_lvl : cfg_max_lvl;

    LOG_I("Conversion parameters: center=(%.10f, %.10f), max_lvl=%d", center_x, center_y, max_lvl);
    LOG_I("Features: texture_compress=%d, meshopt=%d, draco=%d, unlit=%d, top_reconstruct=%d",
          opts.enable_texture_compress, opts.enable_meshopt, opts.enable_draco, opts.enable_unlit,
          opts.enable_top_reconstruct);

    // ============================================================
    // 4. Find and convert all tiles
    // ============================================================
    fs::path data_dir = in_dir / "Data";
    if (!fs::exists(data_dir) || !fs::is_directory(data_dir)) {
        LOG_E("Data directory not found: %s", data_dir.string().c_str());
        return 1;
    }

    struct TileResult {
        std::string stem;                      // tile directory name (e.g. "Tile_-001_+050")
        json tree_json;                        // parsed tile tree (with URIs fixed)
        std::vector<double> box_v;
        std::string coarsest_path;             // for root tile reconstruction
    };

    // ============================================================
    // Helper: parallel tile processing with concurrency limiter
    // ============================================================
    class Semaphore {
        std::mutex mtx;
        std::condition_variable cv;
        size_t count;
    public:
        explicit Semaphore(size_t n) : count(n) {}
        void acquire() {
            std::unique_lock<std::mutex> lock(mtx);
            cv.wait(lock, [&] { return count > 0; });
            --count;
        }
        void release() {
            std::unique_lock<std::mutex> lock(mtx);
            ++count;
            cv.notify_one();
        }
    };

    // Helper: recursively fix content URIs in tile JSON tree
    // so they point to glb files relative to root output directory
    std::function<void(json&, const std::string&)> fix_tile_uris;
    fix_tile_uris = [&fix_tile_uris](json& tile, const std::string& prefix) {
        if (tile.contains("content") && tile["content"].is_object()
            && tile["content"].contains("uri")) {
            std::string uri = tile["content"]["uri"].get<std::string>();
            // uri looks like "./Tile_xxx.glb" — prepend the Data/TileDir/ prefix
            if (uri.size() >= 2 && uri[0] == '.' && (uri[1] == '/' || uri[1] == '\\'))
                tile["content"]["uri"] = "." + prefix + uri.substr(1);
            else
                tile["content"]["uri"] = "." + prefix + uri;
        }
        if (tile.contains("children") && tile["children"].is_array()) {
            for (auto& child : tile["children"])
                fix_tile_uris(child, prefix);
        }
    };

    // ============================================================
    // Phase 1: Build tile trees in parallel (top-level only)
    //           Each thread recursively builds a full osg_tree via get_all_tree().
    // ============================================================
    struct TreeResult {
        osg_tree root;
        std::string stem;
        std::string out_tile_dir;
    };

    std::mutex trees_mutex;
    std::vector<TreeResult> all_trees;

    // Collect tasks first, then dispatch (parallel or serial).
    // Phase 1 is I/O-bound: osgDB::readNodeFiles() has a global mutex,
    // so we cap I/O concurrency to 4 threads even in parallel mode.
    std::vector<std::function<void()>> phase1_tasks;

    for (auto& entry : fs::directory_iterator(data_dir)) {
        if (!entry.is_directory()) continue;

        fs::path tile_dir = entry.path();
        std::string stem = tile_dir.filename().string();
        fs::path osgb_file = tile_dir / (stem + ".osgb");

        if (!fs::exists(osgb_file) || fs::is_directory(osgb_file)) {
            LOG_E("No OSGB file in tile dir: %s", tile_dir.string().c_str());
            continue;
        }

        fs::path out_tile_dir = out_dir / "Data" / stem;
        fs::create_directories(out_tile_dir);

        phase1_tasks.push_back([&trees_mutex, &all_trees,
                                osgb_path = osgb_file.string(),
                                stem,
                                out_path = out_tile_dir.string()]() {
            LOG_I("Phase 1 - Building tree: %s", osgb_path.c_str());
            std::string path_copy = osgb_path;
            osg_tree root = get_all_tree(path_copy);
            if (root.file_name.empty()) {
                LOG_E("Failed to read: %s", osgb_path.c_str());
                return;
            }
            {
                std::lock_guard<std::mutex> lock(trees_mutex);
                all_trees.push_back({std::move(root), stem, out_path});
            }
        });
    }

    if (opts.enable_parallel) {
        unsigned int n_threads = opts.num_threads > 0
            ? (unsigned int)opts.num_threads
            : std::thread::hardware_concurrency();
        unsigned int p1_threads = std::min(n_threads, 4u);  // I/O bound
        Semaphore sem(p1_threads);
        std::vector<std::future<void>> futures;
        futures.reserve(phase1_tasks.size());

        for (auto& task : phase1_tasks) {
            // sem.acquire() INSIDE the lambda — non-blocking submission (Fix #4)
            auto wrapper = [&sem, task = std::move(task)]() {
                sem.acquire();
                task();
                sem.release();
            };
            futures.push_back(std::async(std::launch::async, std::move(wrapper)));
        }
        for (auto& f : futures) f.get();
        LOG_I("Phase 1 complete: %zu tile trees built (parallel, %u I/O threads)",
              all_trees.size(), p1_threads);
    } else {
        for (auto& task : phase1_tasks) task();
        LOG_I("Phase 1 complete: %zu tile trees built (serial)", all_trees.size());
    }

    if (all_trees.empty()) {
        LOG_E("No tile trees were built");
        return 1;
    }

    // ============================================================
    // Phase 2: Flatten all trees → tile conversion (parallel or serial)
    //           Uses cached_node from Phase 1 to avoid redundant
    //           osgDB::readNodeFiles() calls and OSG global mutex.
    //           Tiles are chunked; compute and I/O are separated:
    //           parallel CPU work first, then serialized file writes.
    // ============================================================
    std::vector<FlatTile> all_tiles;
    {
        auto t0 = std::chrono::steady_clock::now();
        for (auto& tr : all_trees) {
            collect_flat_tiles(tr.root, tr.out_tile_dir, all_tiles);
        }
        auto t1 = std::chrono::steady_clock::now();
        LOG_I("Phase 2: Flattened %zu tiles in %.0fms (%s)", all_tiles.size(),
              std::chrono::duration<double, std::milli>(t1 - t0).count(),
              opts.enable_parallel ? "parallel (chunked, serial write)" : "serial");
    }

    if (opts.enable_parallel) {
        const size_t CHUNK_SIZE = 16;
        size_t num_chunks = (all_tiles.size() + CHUNK_SIZE - 1) / CHUNK_SIZE;

        unsigned int p2_threads = opts.num_threads > 0
            ? (unsigned int)opts.num_threads
            : std::thread::hardware_concurrency();
        Semaphore sem(p2_threads);
        std::mutex write_mutex;
        std::atomic<size_t> tiles_done{0};
        std::atomic<size_t> active_chunks{0};
        std::vector<std::future<void>> futures;
        futures.reserve(num_chunks);

        auto t_launch_start = std::chrono::steady_clock::now();
        for (size_t ci = 0; ci < num_chunks; ci++) {
            size_t start = ci * CHUNK_SIZE;
            size_t end = std::min(start + CHUNK_SIZE, all_tiles.size());
            size_t chunk_idx = ci;

            auto chunk_tiles = std::make_shared<std::vector<FlatTile>>(
                all_tiles.begin() + start, all_tiles.begin() + end);

            auto task = [&sem, &opts, chunk_tiles, chunk_idx, num_chunks,
                         total_tiles = all_tiles.size(),
                         &write_mutex, &tiles_done,
                         &active_chunks,
                         first_flag = std::make_shared<std::atomic<bool>>(false)]() {
                sem.acquire();

                // Log the very first chunk to start
                bool expect = false;
                if (first_flag->compare_exchange_strong(expect, true))
                    LOG_I("Phase 2: First chunk started computing (thread is alive)");

                size_t cur_active = active_chunks.fetch_add(1) + 1;
                LOG_I("  Chunk %zu/%zu [%zu tiles] computing... (active=%zu)",
                      chunk_idx + 1, num_chunks, chunk_tiles->size(), cur_active);

                // === Phase 2a: Parallel computation ===
                size_t chunk_size = chunk_tiles->size();
                auto t0 = std::chrono::steady_clock::now();

                struct ChunkResult {
                    std::string glb_buf;
                    std::string out_file;
                    MeshInfo minfo;
                    osg_tree* tree;
                    bool ok;
                };
                std::vector<ChunkResult> results;
                results.reserve(chunk_size);

                for (const auto& tile : *chunk_tiles) {
                    ChunkResult r;
                    r.tree = tile.tree;
                    r.ok = compute_tile_output(tile, opts, r.glb_buf, r.minfo, r.out_file);
                    if (r.ok && r.tree) {
                        r.tree->bbox.max = r.minfo.max;
                        r.tree->bbox.min = r.minfo.min;
                    }
                    results.push_back(std::move(r));
                }

                auto t1 = std::chrono::steady_clock::now();
                double compute_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

                sem.release();

                // === Phase 2b: Serialized file writes ===
                auto t2 = std::chrono::steady_clock::now();
                double wait_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();
                {
                    std::lock_guard<std::mutex> lock(write_mutex);
                    for (auto& r : results) {
                        if (r.ok && !r.glb_buf.empty())
                            write_file(r.out_file.c_str(), r.glb_buf.data(),
                                       (unsigned long)r.glb_buf.size());
                    }
                }

                auto t3 = std::chrono::steady_clock::now();
                double write_ms = std::chrono::duration<double, std::milli>(t3 - t2).count();

                LOG_I("  Chunk %zu/%zu: compute=%.0fms, write_wait=%.0fms, write=%.0fms",
                      chunk_idx + 1, num_chunks, compute_ms, wait_ms, write_ms);

                size_t done = tiles_done.fetch_add(chunk_size) + chunk_size;
                if (done % 200 < chunk_size || done >= total_tiles)
                    LOG_I("  Progress: %zu/%zu tiles done", done, total_tiles);
            };
            futures.push_back(std::async(std::launch::async, std::move(task)));
        }
        auto t_launch_end = std::chrono::steady_clock::now();
        LOG_I("Phase 2: %zu futures launched in %.0fms, waiting for completion...",
              futures.size(),
              std::chrono::duration<double, std::milli>(t_launch_end - t_launch_start).count());
        for (auto& f : futures) f.get();
    } else {
        for (const auto& tile : all_tiles) {
            convert_one_tile_from_cached(tile, opts);
        }
    }

    LOG_I("Phase 2 complete: %zu tiles converted", all_tiles.size());

    // ============================================================
    // Phase 3: Serial aggregation — bbox, geometricError, tile JSON
    //           Must be serial because extend_tile_box()/calc_geometric_error()
    //           are bottom-up on each tree.
    // ============================================================
    std::vector<TileResult> tile_results;
    std::map<std::string, json> tile_jsons_map;  // stem → tree_json (for HLOD leaves)

    for (auto& tr : all_trees) {
        extend_tile_box(tr.root);
        if (tr.root.bbox.max.empty() || tr.root.bbox.min.empty()) {
            LOG_E("Empty bounding box for: %s", tr.stem.c_str());
            continue;
        }

        calc_geometric_error(tr.root);

        std::string json_str = encode_tile_json_1_1(tr.root, degree2rad(center_x), degree2rad(center_y));
        json tile_tree = json::parse(json_str.empty() ? "{}" : json_str);

        std::string uri_prefix = "/Data/" + tr.stem;
        fix_tile_uris(tile_tree, uri_prefix);

        tr.root.bbox.extend(0.2);
        std::vector<double> box_full;
        box_full.insert(box_full.end(), tr.root.bbox.max.begin(), tr.root.bbox.max.end());
        box_full.insert(box_full.end(), tr.root.bbox.min.begin(), tr.root.bbox.min.end());

        std::string coarsest_path = find_coarsest_node(tr.root);

        tile_results.push_back({tr.stem, tile_tree, box_full, coarsest_path});
        tile_jsons_map[tr.stem] = tile_tree;  // For HLOD leaf lookup
    }

    LOG_I("Phase 3 complete: %zu tile trees aggregated", tile_results.size());

    if (tile_results.empty()) {
        LOG_E("No tiles were converted");
        return 1;
    }

    // ============================================================
    // Phase 4: Quadtree HLOD construction (if enabled)
    //           Builds spatial grid → quadtree → merged GLB per level
    // ============================================================
    QuadNode quadtree_root;
    bool has_hlod = false;

    if (opts.enable_top_reconstruct) {
        // Collect data for spatial grid from all_trees
        std::vector<std::string> tile_stems;
        std::vector<std::string> coarsest_paths;
        std::vector<TileBox> tile_bboxes;
        std::vector<double> coarsest_ges;

        for (auto& tr : all_trees) {
            if (tr.root.bbox.max.empty() || tr.root.bbox.min.empty()) continue;
            tile_stems.push_back(tr.stem);

            double coarsest_ge = 0.0;
            std::string coarsest = find_coarsest_node(tr.root, &coarsest_ge);
            coarsest_paths.push_back(coarsest);
            coarsest_ges.push_back(coarsest_ge);

            TileBox bbox;
            bbox.max = tr.root.bbox.max;
            bbox.min = tr.root.bbox.min;
            tile_bboxes.push_back(bbox);
        }

        if (!tile_stems.empty()) {
            // Step 4.1: Build spatial grid
            SpatialGrid grid = build_spatial_grid(tile_stems, coarsest_paths, tile_bboxes, coarsest_ges);

            // Step 4.2: Build quadtree bottom-up
            quadtree_root = build_quadtree(grid);
            has_hlod = quadtree_root.has_content || !quadtree_root.children.empty();

            if (has_hlod) {
                // Create HLOD output directory
                fs::path hlod_dir = out_dir / "Data" / "HLOD";
                std::error_code ec;
                fs::create_directories(hlod_dir, ec);

                // Step 4.3: Bottom-up merge at each quadtree level.
                // Use a recursive lambda to process children first.
                std::function<void(QuadNode&)> merge_node;
                int root_level = quadtree_root.level;
                merge_node = [&](QuadNode& node) {
                    // Process children first (bottom-up)
                    for (auto& child : node.children) {
                        merge_node(child);
                    }

                    // Collect all leaf OSGB paths under this node
                    std::vector<std::string> leaf_paths;
                    collect_leaf_paths(node, grid, leaf_paths);

                    if (leaf_paths.empty()) {
                        LOG_W("HLOD merge: level %d node at (%d,%d) has no leaf paths, skipping",
                              node.level, node.grid_x, node.grid_y);
                        node.has_content = false;
                        return;
                    }

                    // Determine output file name
                    std::string glb_name;
                    if (node.level == root_level) {
                        glb_name = "root.glb";
                    } else {
                        char buf[128];
                        int display_level = root_level - 1 - node.level;
                        snprintf(buf, sizeof(buf), "L%d_X%+04d_Y%+04d.glb",
                                 display_level, node.grid_x, node.grid_y);
                        glb_name = buf;
                    }

                    // Pass node.level + 1 to preserve calc_level_ratio semantics:
                    // level 0 (first merge) → ratio = 0.25, level 1 → 0.0625, etc.
                    LOG_I("HLOD merge: %s (level=%d, %zu leaf paths, grid=(%d,%d) size=%d)",
                          glb_name.c_str(), node.level, leaf_paths.size(),
                          node.grid_x, node.grid_y, node.grid_size);

                    std::string glb_buf;
                    TileBox merged_bbox;
                    bool ok = build_merged_glb(
                        leaf_paths, node.level + 1, glb_buf, merged_bbox,
                        opts.enable_texture_compress, opts.enable_meshopt,
                        opts.enable_draco, opts.enable_unlit,
                        opts.top_texture_max_size, opts.simplify_ratio,
                        opts.draco_pos_bits, opts.draco_normal_bits, opts.draco_uv_bits,
                        opts.ktx2_quality);

                    if (ok && !glb_buf.empty()) {
                        fs::path glb_path = hlod_dir / glb_name;
                        if (write_file(glb_path.string().c_str(),
                                       glb_buf.data(), (unsigned long)glb_buf.size())) {
                            node.bbox = merged_bbox;
                            node.glb_uri = "./Data/HLOD/" + glb_name;
                            node.has_content = true;
                            LOG_I("  written: %s (%zu bytes)", glb_path.string().c_str(), glb_buf.size());
                        } else {
                            LOG_E("  failed to write: %s", glb_path.string().c_str());
                            node.has_content = false;
                        }
                    } else {
                        LOG_W("  merge failed for level %d node at (%d,%d)",
                              node.level, node.grid_x, node.grid_y);
                        node.has_content = false;
                    }
                };

                merge_node(quadtree_root);

                LOG_I("Phase 4 complete: HLOD quadtree built (%d levels)", root_level);
            } else {
                LOG_W("Quadtree build returned empty root, falling back to flat tileset");
            }
        } else {
            LOG_W("No tile stems collected, skipping HLOD build");
        }
    }

    // ============================================================
    // Helper: recursively rewrite content URIs in a tile JSON tree
    // for external sub-tilesets. Since sub-tilesets live in
    // ./subtilesets/ (one level below root), all "./Data/..." URIs
    // need to become "../Data/..." so they resolve correctly.
    // ============================================================
    std::function<void(json&)> rewrite_uris_for_sub_tileset;
    rewrite_uris_for_sub_tileset = [&rewrite_uris_for_sub_tileset](json& tile) {
        if (tile.contains("content") && tile["content"].is_object()
            && tile["content"].contains("uri")) {
            std::string uri = tile["content"]["uri"].get<std::string>();
            // Replace "./Data/" → "../Data/" so URIs resolve from subtilesets/
            if (uri.size() >= 2 && uri[0] == '.' && uri[1] == '/') {
                tile["content"]["uri"] = ".." + uri.substr(1);
            }
        }
        if (tile.contains("children") && tile["children"].is_array()) {
            for (auto& child : tile["children"])
                rewrite_uris_for_sub_tileset(child);
        }
    };

    // ============================================================
    // 5. Build tileset.json
    // ============================================================
    // Compute root bounding box (from tile_results for flat mode, from quadtree for HLOD)
    std::vector<double> root_box = {-1e38, -1e38, -1e38, 1e38, 1e38, 1e38};
    double root_ge = 0.0;

    if (has_hlod) {
        // Use quadtree root bbox
        for (int i = 0; i < 3; i++) {
            root_box[i] = quadtree_root.bbox.max[i];
            root_box[i+3] = quadtree_root.bbox.min[i];
        }
        root_ge = quadtree_root.geometricError;
    } else {
        for (auto& tr : tile_results) {
            for (int i = 0; i < 3; i++) {
                if (tr.box_v[i] > root_box[i]) root_box[i] = tr.box_v[i];
            }
            for (int i = 3; i < 6; i++) {
                if (tr.box_v[i] < root_box[i]) root_box[i] = tr.box_v[i];
            }
            if (tr.tree_json.contains("geometricError"))
                root_ge = std::max(root_ge, tr.tree_json["geometricError"].get<double>());
        }
    }

    // Compute transform matrix
    double trans_height = 0.0;
    if (origin_height.has_value()) {
        trans_height = *origin_height;
    } else if (enu_offset.has_value()) {
        trans_height = std::get<2>(*enu_offset);
    } else if (opts.has_region_offset) {
        trans_height = opts.region_offset - root_box[5];
    }

    std::vector<double> trans_vec = transfrom_xyz(center_x, center_y, trans_height);

    // Apply ENU offset to translation if applicable
    if (enu_offset.has_value()) {
        double eox = std::get<0>(*enu_offset), eoy = std::get<1>(*enu_offset), eoz = std::get<2>(*enu_offset);
        double lat_rad = degree2rad(center_y), lon_rad = degree2rad(center_x);
        double sinLat = std::sin(lat_rad), cosLat = std::cos(lat_rad);
        double sinLon = std::sin(lon_rad), cosLon = std::cos(lon_rad);

        double ecx = -sinLon * eox - sinLat * cosLon * eoy + cosLat * cosLon * eoz;
        double ecy =  cosLon * eox - sinLat * sinLon * eoy + cosLat * sinLon * eoz;
        double ecz =  cosLat * eoy + sinLat * eoz;

        trans_vec[12] += ecx;
        trans_vec[13] += ecy;
        trans_vec[14] += ecz;
    }

    fprintf(stderr, "[transform] lon=%.10f lat=%.10f h=%.3f -> ECEF: x=%.10f y=%.10f z=%.10f\n",
            center_x, center_y, trans_height, trans_vec[12], trans_vec[13], trans_vec[14]);

    // ============================================================
    // Build tileset.json
    // ============================================================
    json root_tileset;
    root_tileset["asset"]["version"] = "1.1";
    root_tileset["asset"]["gltfUpAxis"] = "Z";
    root_tileset["extensionsUsed"] = json::array({"3DTILES_content_gltf"});
    root_tileset["extensionsRequired"] = json::array({"3DTILES_content_gltf"});

    if (has_hlod) {
        // HLOD mode: root tile is the quadtree root.
        // When split_json is enabled, writes sub-tilesets at the specified
        // display level and returns reference tiles instead of full subtrees.
        int split_d = opts.enable_split_json ? opts.split_depth : 0;
        bool do_split = (split_d > 0);
        std::string out_dir_str = do_split ? out_dir.string() : "";
        json quadtree_json = encode_quadtree_json(
            quadtree_root, tile_jsons_map,
            0,                       // current_display_level (root = 0)
            quadtree_root.level,     // root_level of quadtree
            split_d,
            out_dir_str,
            do_split);               // externalize_pagedlod = true when split enabled

        // Add ECEF transform to root
        quadtree_json["transform"] = trans_vec;
        quadtree_json["boundingVolume"]["box"] = box_to_tileset_box(root_box);

        // Root GE computed from quadtree hierarchy (no cap)
        quadtree_json["geometricError"] = root_ge;
        root_tileset["geometricError"] = root_ge;

        root_tileset["root"] = quadtree_json;
    } else {
        // Flat mode (no HLOD)
        if (opts.enable_split_json) {
            // --- Split mode: one external sub-tileset per top-level tree ---
            fs::path sub_dir = out_dir / "subtilesets";
            fs::create_directories(sub_dir);

            json root_tile;
            root_tile["transform"] = trans_vec;
            root_tile["boundingVolume"]["box"] = box_to_tileset_box(root_box);
            root_tile["refine"] = "REPLACE";
            root_tile["geometricError"] = std::min(root_ge, 2000.0);
            root_tileset["geometricError"] = std::min(root_ge, 2000.0);
            root_tile["children"] = json::array();

            for (auto& tr : tile_results) {
                // Deep-copy the tree JSON so we can rewrite URIs without
                // affecting the HLOD leaf lookup (tile_jsons_map).
                json tree_copy = tr.tree_json;
                rewrite_uris_for_sub_tileset(tree_copy);

                // Build sub-tileset envelope
                json sub_tileset;
                sub_tileset["asset"]["version"] = "1.1";
                sub_tileset["extensionsUsed"] = json::array({"3DTILES_content_gltf"});
                sub_tileset["extensionsRequired"] = json::array({"3DTILES_content_gltf"});
                sub_tileset["geometricError"] = tree_copy.value("geometricError", 0.0);
                sub_tileset["root"] = tree_copy;

                // Write subtilesets/<stem>.json
                std::string stem = tr.stem;
                fs::path sub_path = sub_dir / (stem + ".json");
                std::ofstream sub_ofs(sub_path);
                sub_ofs << sub_tileset.dump(2);
                sub_ofs.close();
                LOG_I("  Wrote sub-tileset: subtilesets/%s.json", stem.c_str());

                // Build lightweight reference tile for root tileset.json
                json ref_tile;
                ref_tile["boundingVolume"]["box"] = box_to_tileset_box(tr.box_v);
                ref_tile["content"]["uri"] = "./subtilesets/" + stem + ".json";
                ref_tile["geometricError"] = tr.tree_json.value("geometricError", 0.0);
                ref_tile["refine"] = "REPLACE";
                root_tile["children"].push_back(ref_tile);
            }

            root_tileset["root"] = root_tile;
        } else {
            // --- Original monolithic mode ---
            json root_tile;
            root_tile["transform"] = trans_vec;
            root_tile["boundingVolume"]["box"] = box_to_tileset_box(root_box);
            root_tile["refine"] = "REPLACE";
            root_tile["geometricError"] = std::min(root_ge, 2000.0);
            root_tileset["geometricError"] = std::min(root_ge, 2000.0);
            root_tile["children"] = json::array();

            for (auto& tr : tile_results) {
                root_tile["children"].push_back(tr.tree_json);
            }

            root_tileset["root"] = root_tile;
        }
    }

    fs::path root_json_path = out_dir / "tileset.json";
    std::ofstream root_ofs(root_json_path);
    root_ofs << root_tileset.dump(2);
    root_ofs.close();

    // Cleanup
    auto* t = GetGlobalTransformer();
    if (t) { delete t; SetGlobalTransformer(nullptr); }

    auto elapsed = duration_cast<duration<double>>(high_resolution_clock::now() - tick);
    LOG_I("Conversion complete. Time: %.2f s", elapsed.count());
    return 0;
}

} // namespace osgb_converter
