// JSON-only runtime optimizer for an existing 3D Tiles output.
//
// It never copies or changes Data/*.glb. Optimized JSON files are written to
// a separate directory and their relative content URIs are rewritten back to
// the original Data directory.

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

constexpr double kInvalidCoordinateLimit = 1.0e30;

struct Options {
    fs::path input_dir;
    fs::path output_dir;
    double max_hlod_mb = 4.0;
    double index_geometric_error = 1.0e12;
    bool flatten_index = true;
    bool dry_run = false;
    bool in_place = false;
    bool pretty = false;
};

struct Stats {
    std::uint64_t json_files = 0;
    std::uint64_t tiles = 0;
    std::uint64_t invalid_boxes = 0;
    std::uint64_t repaired_boxes = 0;
    std::uint64_t unrepaired_boxes = 0;
    std::uint64_t oversized_hlod_removed = 0;
    std::uint64_t oversized_hlod_bytes = 0;
    std::uint64_t index_tiles_forced = 0;
    std::uint64_t index_levels_flattened = 0;
    std::uint64_t rewritten_uris = 0;
};

struct Document {
    fs::path source;
    fs::path relative;
    json value;
    bool optimized = false;
    bool optimizing = false;
};

Options g_options;
Stats g_stats;
std::unordered_map<std::string, Document> g_documents;

std::string path_key(const fs::path& path) {
    std::error_code ec;
    fs::path p = fs::weakly_canonical(path, ec);
    if (ec) p = fs::absolute(path, ec).lexically_normal();
    std::string key = p.generic_string();
#ifdef _WIN32
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#endif
    return key;
}

std::string lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool is_remote_or_embedded_uri(const std::string& uri) {
    const std::string lower = lower_copy(uri);
    return lower.find("://") != std::string::npos
        || lower.rfind("data:", 0) == 0
        || lower.rfind("blob:", 0) == 0;
}

std::string uri_path_part(const std::string& uri) {
    const auto pos = uri.find_first_of("?#");
    return uri.substr(0, pos);
}

fs::path resolve_uri(const fs::path& document, const std::string& uri) {
    std::string path_text = uri_path_part(uri);
    return (document.parent_path() / fs::path(path_text)).lexically_normal();
}

bool content_uri(const json& tile, std::string& uri) {
    if (!tile.contains("content") || !tile["content"].is_object()) return false;
    const auto& content = tile["content"];
    const char* key = content.contains("uri") ? "uri"
                    : content.contains("url") ? "url" : nullptr;
    if (!key || !content[key].is_string()) return false;
    uri = content[key].get<std::string>();
    return !uri.empty();
}

bool has_content(const json& tile) {
    std::string ignored;
    return content_uri(tile, ignored);
}

bool has_children(const json& tile) {
    return tile.contains("children")
        && tile["children"].is_array()
        && !tile["children"].empty();
}

bool valid_box(const json& box) {
    if (!box.is_array() || box.size() != 12) return false;
    for (const auto& item : box) {
        if (!item.is_number()) return false;
        const double value = item.get<double>();
        if (!std::isfinite(value) || std::abs(value) >= kInvalidCoordinateLimit)
            return false;
    }
    return true;
}

struct Aabb {
    std::array<double, 3> min {
        std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::infinity()
    };
    std::array<double, 3> max {
        -std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity()
    };
    bool valid = false;
};

bool include_box(Aabb& aabb, const json& box) {
    if (!valid_box(box)) return false;
    for (int axis = 0; axis < 3; ++axis) {
        const double center = box[axis].get<double>();
        const double extent =
            std::abs(box[3 + axis].get<double>())
            + std::abs(box[6 + axis].get<double>())
            + std::abs(box[9 + axis].get<double>());
        aabb.min[axis] = std::min(aabb.min[axis], center - extent);
        aabb.max[axis] = std::max(aabb.max[axis], center + extent);
    }
    aabb.valid = true;
    return true;
}

json aabb_to_box(const Aabb& aabb) {
    json box = json::array();
    const double cx = (aabb.min[0] + aabb.max[0]) * 0.5;
    const double cy = (aabb.min[1] + aabb.max[1]) * 0.5;
    const double cz = (aabb.min[2] + aabb.max[2]) * 0.5;
    const double hx = (aabb.max[0] - aabb.min[0]) * 0.5;
    const double hy = (aabb.max[1] - aabb.min[1]) * 0.5;
    const double hz = (aabb.max[2] - aabb.min[2]) * 0.5;
    box = {cx, cy, cz, hx, 0.0, 0.0, 0.0, hy, 0.0, 0.0, 0.0, hz};
    return box;
}

bool is_json_uri(const std::string& uri) {
    return lower_copy(fs::path(uri_path_part(uri)).extension().string()) == ".json";
}

bool is_glb_uri(const std::string& uri) {
    return lower_copy(fs::path(uri_path_part(uri)).extension().string()) == ".glb";
}

bool is_hlod_path(const fs::path& path) {
    const std::string lower = lower_copy(path.generic_string());
    return lower.find("/hlod/") != std::string::npos
        || lower.find("\\hlod\\") != std::string::npos;
}

Document* find_external_document(const Document& owner, const std::string& uri) {
    if (!is_json_uri(uri) || is_remote_or_embedded_uri(uri)) return nullptr;
    auto it = g_documents.find(path_key(resolve_uri(owner.source, uri)));
    return it == g_documents.end() ? nullptr : &it->second;
}

void optimize_document(Document& document);

void flatten_index_children(json& tile) {
    if (!g_options.flatten_index || !has_children(tile)) return;

    json flattened = json::array();
    for (auto& child : tile["children"]) {
        const bool promotable =
            !has_content(child)
            && !child.contains("transform")
            && has_children(child);
        if (promotable) {
            for (auto& grandchild : child["children"])
                flattened.push_back(std::move(grandchild));
            ++g_stats.index_levels_flattened;
        } else {
            flattened.push_back(std::move(child));
        }
    }
    tile["children"] = std::move(flattened);
}

void optimize_tile(json& tile, Document& owner) {
    ++g_stats.tiles;

    if (tile.contains("children") && tile["children"].is_array()) {
        for (auto& child : tile["children"])
            optimize_tile(child, owner);
    }

    std::string uri;
    Document* external = nullptr;
    if (content_uri(tile, uri))
        external = find_external_document(owner, uri);
    if (external)
        optimize_document(*external);

    // Oversized HLOD content causes a large decode/upload/draw spike. It is
    // safe to omit only when descendants can replace it.
    if (g_options.max_hlod_mb > 0.0
        && has_children(tile)
        && content_uri(tile, uri)
        && is_glb_uri(uri)
        && !is_remote_or_embedded_uri(uri)) {
        const fs::path glb_path = resolve_uri(owner.source, uri);
        std::error_code ec;
        const auto size = fs::file_size(glb_path, ec);
        const auto limit = static_cast<std::uintmax_t>(
            g_options.max_hlod_mb * 1024.0 * 1024.0);
        if (!ec && is_hlod_path(glb_path) && size > limit) {
            tile.erase("content");
            ++g_stats.oversized_hlod_removed;
            g_stats.oversized_hlod_bytes += size;
        }
    }

    flatten_index_children(tile);

    json* box = nullptr;
    if (tile.contains("boundingVolume") && tile["boundingVolume"].is_object()
        && tile["boundingVolume"].contains("box")) {
        box = &tile["boundingVolume"]["box"];
    }

    if (!box || !valid_box(*box)) {
        ++g_stats.invalid_boxes;
        Aabb combined;

        if (tile.contains("children") && tile["children"].is_array()) {
            for (const auto& child : tile["children"]) {
                if (child.contains("boundingVolume")
                    && child["boundingVolume"].is_object()
                    && child["boundingVolume"].contains("box")) {
                    include_box(combined, child["boundingVolume"]["box"]);
                }
            }
        }

        // An external tileset reference has no inline children. Its optimized
        // root bounding volume is the best replacement for the reference box.
        if (!combined.valid && external
            && external->value.contains("root")
            && external->value["root"].contains("boundingVolume")
            && external->value["root"]["boundingVolume"].contains("box")) {
            include_box(combined,
                        external->value["root"]["boundingVolume"]["box"]);
        }

        if (combined.valid) {
            tile["boundingVolume"]["box"] = aabb_to_box(combined);
            ++g_stats.repaired_boxes;
        } else {
            ++g_stats.unrepaired_boxes;
        }
    }

    if (!has_content(tile) && has_children(tile)) {
        double current = 0.0;
        if (tile.contains("geometricError")
            && tile["geometricError"].is_number()) {
            current = tile["geometricError"].get<double>();
        }
        if (!std::isfinite(current)
            || current < g_options.index_geometric_error) {
            tile["geometricError"] = g_options.index_geometric_error;
            ++g_stats.index_tiles_forced;
        }
        tile["refine"] = "REPLACE";
    }
}

void optimize_document(Document& document) {
    if (document.optimized) return;
    if (document.optimizing) {
        std::cerr << "[WARN] external tileset cycle at " << document.source << "\n";
        return;
    }
    document.optimizing = true;
    if (document.value.contains("root") && document.value["root"].is_object())
        optimize_tile(document.value["root"], document);
    document.optimizing = false;
    document.optimized = true;
}

void rewrite_tile_uris(json& tile, const Document& document,
                       const fs::path& destination) {
    std::string uri;
    if (content_uri(tile, uri) && !is_remote_or_embedded_uri(uri)) {
        const fs::path source_target = resolve_uri(document.source, uri);
        fs::path destination_target = source_target;

        auto doc_it = g_documents.find(path_key(source_target));
        if (doc_it != g_documents.end())
            destination_target = g_options.output_dir / doc_it->second.relative;

        // Both paths are absolute and normalized by load/argument handling.
        // lexically_relative is deterministic here and, unlike fs::relative,
        // does not depend on every intermediate output path already existing.
        fs::path relative =
            destination_target.lexically_relative(destination.parent_path());
        if (!relative.empty() && !relative.is_absolute()) {
            std::string rewritten = relative.generic_string();
            if (rewritten.rfind("../", 0) != 0)
                rewritten = "./" + rewritten;

            auto& content = tile["content"];
            const char* key = content.contains("uri") ? "uri" : "url";
            if (rewritten != uri) {
                content[key] = rewritten;
                ++g_stats.rewritten_uris;
            }
        }
    }

    if (tile.contains("children") && tile["children"].is_array()) {
        for (auto& child : tile["children"])
            rewrite_tile_uris(child, document, destination);
    }
}

json read_json(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open " + path.string());
    json value;
    input >> value;
    return value;
}

void write_json(const fs::path& path, const json& value) {
    fs::create_directories(path.parent_path());
    const fs::path temporary = path.string() + ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("cannot write " + temporary.string());
        output << (g_options.pretty ? value.dump(2) : value.dump());
    }
    std::error_code ec;
    fs::rename(temporary, path, ec);
    if (ec) {
        fs::remove(path, ec);
        ec.clear();
        fs::rename(temporary, path, ec);
    }
    if (ec) throw std::runtime_error("cannot finalize " + path.string());
}

void print_usage() {
    std::cout << R"(Usage:
  tileset_json_optimizer -i <tileset-dir> -o <json-overlay-dir> [options]
  tileset_json_optimizer -i <tileset-dir> --in-place [options]
  tileset_json_optimizer -i <tileset-dir> --dry-run [options]

Options:
  -i, --input DIR          Existing output containing tileset.json
  -o, --output DIR         New JSON-only overlay directory
  --max-hlod-mb N          Remove HLOD GLB references above N MB (default: 4)
  --index-ge N             GE for content-less index tiles (default: 1e12)
  --no-flatten-index       Keep nested content-less index levels
  --in-place               Atomically replace JSON under the input directory
  --pretty                 Pretty-print output JSON
  --dry-run                Analyze and optimize in memory without writing
  -h, --help               Show this help

The output contains JSON only. Serve the common parent of the original and
overlay directories so ../original/Data/... URIs remain reachable.
)";
}

bool parse_arguments(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if ((arg == "-i" || arg == "--input") && i + 1 < argc) {
            g_options.input_dir = argv[++i];
        } else if ((arg == "-o" || arg == "--output") && i + 1 < argc) {
            g_options.output_dir = argv[++i];
        } else if (arg == "--max-hlod-mb" && i + 1 < argc) {
            g_options.max_hlod_mb = std::stod(argv[++i]);
        } else if (arg == "--index-ge" && i + 1 < argc) {
            g_options.index_geometric_error = std::stod(argv[++i]);
        } else if (arg == "--no-flatten-index") {
            g_options.flatten_index = false;
        } else if (arg == "--in-place") {
            g_options.in_place = true;
        } else if (arg == "--pretty") {
            g_options.pretty = true;
        } else if (arg == "--dry-run") {
            g_options.dry_run = true;
        } else if (arg == "-h" || arg == "--help") {
            print_usage();
            return false;
        } else {
            throw std::runtime_error("unknown argument: " + arg);
        }
    }

    if (g_options.input_dir.empty())
        throw std::runtime_error("--input is required");
    if (g_options.in_place && !g_options.output_dir.empty())
        throw std::runtime_error("--in-place cannot be combined with --output");
    if (!g_options.dry_run && !g_options.in_place && g_options.output_dir.empty())
        throw std::runtime_error("--output is required unless --dry-run is used");
    if (g_options.max_hlod_mb < 0.0)
        throw std::runtime_error("--max-hlod-mb cannot be negative");
    if (!(g_options.index_geometric_error > 0.0)
        || !std::isfinite(g_options.index_geometric_error))
        throw std::runtime_error("--index-ge must be finite and positive");
    return true;
}

void load_documents() {
    g_options.input_dir = fs::weakly_canonical(g_options.input_dir);
    const fs::path root_json = g_options.input_dir / "tileset.json";
    if (!fs::is_regular_file(root_json))
        throw std::runtime_error("tileset.json not found under input directory");

    std::vector<fs::path> files {root_json};
    const fs::path subtilesets = g_options.input_dir / "subtilesets";
    if (fs::is_directory(subtilesets)) {
        for (const auto& entry : fs::recursive_directory_iterator(subtilesets)) {
            if (entry.is_regular_file()
                && lower_copy(entry.path().extension().string()) == ".json") {
                files.push_back(entry.path());
            }
        }
    }

    for (const auto& file : files) {
        Document document;
        document.source = fs::weakly_canonical(file);
        document.relative = fs::relative(document.source, g_options.input_dir);
        document.value = read_json(document.source);
        g_documents.emplace(path_key(document.source), std::move(document));
    }
    g_stats.json_files = g_documents.size();
}

void verify_output_directory() {
    if (g_options.dry_run) return;
    if (g_options.in_place) {
        g_options.output_dir = g_options.input_dir;
        return;
    }
    g_options.output_dir = fs::absolute(g_options.output_dir).lexically_normal();
    if (path_key(g_options.output_dir) == path_key(g_options.input_dir))
        throw std::runtime_error("output must differ from input");

    if (fs::exists(g_options.output_dir)) {
        if (!fs::is_directory(g_options.output_dir)
            || fs::directory_iterator(g_options.output_dir)
                != fs::directory_iterator()) {
            throw std::runtime_error("output directory already exists and is not empty");
        }
    } else {
        fs::create_directories(g_options.output_dir);
    }
}

void print_stats() {
    std::cout
        << "\nJSON optimization summary\n"
        << "  JSON files:                " << g_stats.json_files << "\n"
        << "  Tiles visited:             " << g_stats.tiles << "\n"
        << "  Invalid boxes found:       " << g_stats.invalid_boxes << "\n"
        << "  Invalid boxes repaired:    " << g_stats.repaired_boxes << "\n"
        << "  Invalid boxes unrepaired:  " << g_stats.unrepaired_boxes << "\n"
        << "  Oversized HLOD removed:    " << g_stats.oversized_hlod_removed << "\n"
        << "  HLOD references avoided:   "
        << (g_stats.oversized_hlod_bytes / (1024.0 * 1024.0)) << " MB\n"
        << "  Index tiles forced:        " << g_stats.index_tiles_forced << "\n"
        << "  Index levels flattened:    " << g_stats.index_levels_flattened << "\n"
        << "  Content URIs rewritten:    " << g_stats.rewritten_uris << "\n";
}

} // namespace

int main(int argc, char* argv[]) {
    try {
        if (!parse_arguments(argc, argv)) return 0;
        load_documents();
        verify_output_directory();

        auto root_it = g_documents.find(
            path_key(g_options.input_dir / "tileset.json"));
        if (root_it == g_documents.end())
            throw std::runtime_error("root document was not loaded");
        optimize_document(root_it->second);

        // Optimize unreferenced JSON files too, so the overlay is complete.
        for (auto& [key, document] : g_documents)
            optimize_document(document);

        if (!g_options.dry_run) {
            for (auto& [key, document] : g_documents) {
                const fs::path destination =
                    g_options.output_dir / document.relative;
                if (document.value.contains("root")
                    && document.value["root"].is_object()) {
                    rewrite_tile_uris(
                        document.value["root"], document, destination);
                }
                write_json(destination, document.value);
            }
            std::cout << (g_options.in_place
                ? "[OK] Input JSON files replaced in place: "
                : "[OK] JSON overlay written to: ")
                      << g_options.output_dir << "\n";
        }

        print_stats();
        return g_stats.unrepaired_boxes == 0 ? 0 : 2;
    } catch (const std::exception& error) {
        std::cerr << "[ERROR] " << error.what() << "\n";
        return 1;
    }
}
