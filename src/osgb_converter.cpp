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
#include <chrono>
#include <thread>
#include <functional>
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
    LOG_I("Features: texture_compress=%d, meshopt=%d, draco=%d, unlit=%d",
          opts.enable_texture_compress, opts.enable_meshopt, opts.enable_draco, opts.enable_unlit);

    // ============================================================
    // 4. Find and convert all tiles
    // ============================================================
    fs::path data_dir = in_dir / "Data";
    if (!fs::exists(data_dir) || !fs::is_directory(data_dir)) {
        LOG_E("Data directory not found: %s", data_dir.string().c_str());
        return 1;
    }

    struct TileResult {
        json tree_json;                        // parsed tile tree (with URIs fixed)
        std::vector<double> box_v;
    };

    // Helper: recursively fix content URIs in tile JSON tree
    // so they point to b3dm files relative to root output directory
    std::function<void(json&, const std::string&)> fix_tile_uris;
    fix_tile_uris = [&fix_tile_uris](json& tile, const std::string& prefix) {
        if (tile.contains("content") && tile["content"].is_object()
            && tile["content"].contains("uri")) {
            std::string uri = tile["content"]["uri"].get<std::string>();
            // uri looks like "./Tile_xxx.b3dm" — prepend the Data/TileDir/ prefix
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

    std::vector<TileResult> tile_results;

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

        std::string osgb_path = osgb_file.string();
        LOG_I("Processing: %s", osgb_path.c_str());

        // Build OSGB tree
        std::string path_copy = osgb_path;
        osg_tree root = get_all_tree(path_copy);
        if (root.file_name.empty()) {
            LOG_E("Failed to read: %s", osgb_path.c_str());
            continue;
        }

        // Convert all tiles in this tree
        do_tile_job(root, out_tile_dir.string(), max_lvl,
                    opts.enable_texture_compress, opts.enable_meshopt,
                    opts.enable_draco, opts.enable_unlit);

        // Compute bounding box and geometric error
        extend_tile_box(root);
        if (root.bbox.max.empty() || root.bbox.min.empty()) {
            LOG_E("Empty bounding box for: %s", osgb_path.c_str());
            continue;
        }

        calc_geometric_error(root);

        // Generate tile JSON, parse it, and fix URIs to point to b3dm files
        // relative to the root output directory
        std::string json_str = encode_tile_json(root, degree2rad(center_x), degree2rad(center_y));
        json tile_tree = json::parse(json_str.empty() ? "{}" : json_str);

        // Prefix: "/Data/Tile_-051_+050" so URIs become "./Data/Tile_-051_+050/Tile_xxx.b3dm"
        std::string uri_prefix = "/Data/" + stem;
        fix_tile_uris(tile_tree, uri_prefix);

        std::vector<double> box_v;
        box_v.insert(box_v.end(), root.bbox.max.begin(), root.bbox.max.end());
        box_v.insert(box_v.end(), root.bbox.min.begin(), root.bbox.min.end());

        root.bbox.extend(0.2);
        std::vector<double> box_full;
        box_full.insert(box_full.end(), root.bbox.max.begin(), root.bbox.max.end());
        box_full.insert(box_full.end(), root.bbox.min.begin(), root.bbox.min.end());

        tile_results.push_back({tile_tree, box_full});
    }

    if (tile_results.empty()) {
        LOG_E("No tiles were converted");
        return 1;
    }

    // ============================================================
    // 5. Build single tileset.json (flat structure, no sub-tileset.json)
    // ============================================================
    std::vector<double> root_box = {-1e38, -1e38, -1e38, 1e38, 1e38, 1e38};
    double root_ge = 0.0;

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
    // Build single tileset.json — all tile trees embedded directly
    // ============================================================
    json root_tileset;
    root_tileset["asset"]["version"] = "1.0";
    root_tileset["asset"]["gltfUpAxis"] = "Z";
    root_tileset["geometricError"] = root_ge * 2.0;

    json root_tile;
    root_tile["transform"] = trans_vec;
    root_tile["boundingVolume"]["box"] = box_to_tileset_box(root_box);
    root_tile["geometricError"] = root_ge * 2.0;
    root_tile["refine"] = "REPLACE";
    root_tile["children"] = json::array();

    // Embed each top-level tile tree directly (instead of referencing sub-tileset.json)
    for (auto& tr : tile_results) {
        root_tile["children"].push_back(tr.tree_json);
    }

    root_tileset["root"] = root_tile;

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
