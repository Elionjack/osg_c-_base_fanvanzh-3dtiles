/// tileset_hollow — hollow out LOD levels in 3D Tiles 1.1 tileset JSON
///
/// Post-processes src1.1 output directory:
///   1. Copies Data/ -> output
///   2. Processes subtilesets/Tile_*.json, removes content URI for specified
///      LOD levels, geometricError inherited top-down from nearest real ancestor
///   3. HLOD_*.json skipped by default (use --hollow-hlod to process them)
///   4. tileset.json copied as-is
///
/// Usage:
///   tileset_hollow -i <input_dir> -o <output_dir> --levels 15-21
///   tileset_hollow -i <input_dir> -o <output_dir> --levels 15,16,18,20 --hollow-hlod

#include "tileset_hollow.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

// ===========================================================================
// helpers
// ===========================================================================

static void copy_data_dir(const fs::path& from, const fs::path& to) {
    if (!fs::exists(from)) {
        std::cerr << "[WARN] Data/ not found: " << from << "\n";
        return;
    }
    fs::create_directories(to);
    for (const auto& entry : fs::recursive_directory_iterator(from)) {
        auto rel = fs::relative(entry.path(), from);
        auto dst = to / rel;
        if (entry.is_directory()) {
            fs::create_directories(dst);
        } else {
            fs::copy_file(entry.path(), dst, fs::copy_options::overwrite_existing);
        }
    }
    std::cout << "[OK] Data/ copied\n";
}

static bool is_hlod_file(const std::string& stem) {
    return stem.find("HLOD_") != std::string::npos;
}

static std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::cerr << "[ERROR] cannot open: " << path << "\n";
        return {};
    }
    std::ostringstream oss;
    oss << in.rdbuf();
    return oss.str();
}

static void write_file(const fs::path& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        std::cerr << "[ERROR] cannot write: " << path << "\n";
        return;
    }
    out << content;
}

// ===========================================================================
// main
// ===========================================================================

struct Options {
    fs::path input_dir;
    fs::path output_dir;
    bool hollow_hlod = false;
    bool no_copy_data = false;
    std::unordered_set<int> levels;
};

static void print_usage() {
    std::cout << R"(Usage: tileset_hollow -i <input_dir> -o <output_dir> --levels <levels> [options]

Options:
  -i, --input DIR      Input tileset dir (with tileset.json + subtilesets/ + Data/)
  -o, --output DIR     Output directory
  --levels LEVELS      Hollow these LOD levels. Supports:
                          single:   15
                          list:     15,16,17,20
                          range:    15-21
                          mixed:    15-18,20,22
  --hollow-hlod        Also process HLOD_*.json (default: Tile_*.json only)
  --no-copy-data       Skip Data/ copy (link Data/ manually)

Example:
  tileset_hollow -i output_dir -o hollowed --levels 15-21
  tileset_hollow -i output_dir -o hollowed --levels 15,16,18,20 --hollow-hlod
)";
}

static bool parse_args(int argc, char* argv[], Options& opt) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "-i" || arg == "--input") && i + 1 < argc) {
            opt.input_dir = argv[++i];
        } else if ((arg == "-o" || arg == "--output") && i + 1 < argc) {
            opt.output_dir = argv[++i];
        } else if (arg == "--levels" && i + 1 < argc) {
            opt.levels = parse_levels(argv[++i]);
        } else if (arg == "--hollow-hlod") {
            opt.hollow_hlod = true;
        } else if (arg == "--no-copy-data") {
            opt.no_copy_data = true;
        } else if (arg == "-h" || arg == "--help") {
            print_usage();
            return false;
        } else {
            std::cerr << "[ERROR] unknown argument: " << arg << "\n";
            print_usage();
            return false;
        }
    }

    if (opt.input_dir.empty() || opt.output_dir.empty() || opt.levels.empty()) {
        std::cerr << "[ERROR] -i, -o, and --levels are all required\n";
        print_usage();
        return false;
    }
    return true;
}

// ===========================================================================
// entry
// ===========================================================================

int main(int argc, char* argv[])
{
    Options opt;
    if (!parse_args(argc, argv, opt)) return 1;

    if (!fs::exists(opt.input_dir) || !fs::is_directory(opt.input_dir)) {
        std::cerr << "[ERROR] input directory not found: " << opt.input_dir << "\n";
        return 1;
    }

    // Print hollow levels
    std::cout << "Hollow levels: ";
    for (int lv : opt.levels) std::cout << lv << " ";
    std::cout << "\n";

    // ---- 1. Copy Data/ ---------------------------------------------------
    if (!opt.no_copy_data) {
        copy_data_dir(opt.input_dir / "Data", opt.output_dir / "Data");
    } else {
        std::cout << "[SKIP] Data/ not copied (--no-copy-data)\n";
    }

    // ---- 2. Process subtilesets/ --------------------------------------------
    fs::path subs_in  = opt.input_dir / "subtilesets";
    fs::path subs_out = opt.output_dir / "subtilesets";
    fs::create_directories(subs_out);

    int processed = 0, skipped = 0;

    if (fs::exists(subs_in) && fs::is_directory(subs_in)) {
        for (const auto& entry : fs::directory_iterator(subs_in)) {
            if (!entry.is_regular_file()) continue;
            if (entry.path().extension() != ".json") continue;

            std::string stem = entry.path().stem().string();

            // HLOD files: copy as-is unless --hollow-hlod
            if (is_hlod_file(stem) && !opt.hollow_hlod) {
                fs::copy_file(entry.path(), subs_out / entry.path().filename(),
                              fs::copy_options::overwrite_existing);
                ++skipped;
                continue;
            }

            // Read JSON
            std::string raw = read_file(entry.path());
            if (raw.empty()) continue;

            auto j = nlohmann::json::parse(raw, nullptr, false);
            if (j.is_discarded()) {
                std::cerr << "[WARN] invalid JSON: " << entry.path().filename() << "\n";
                continue;
            }

            // Initial effective_ge from tileset-level
            double initial_ge = j.value("geometricError", 1000.0);

            // Process recursively
            process_node(j["root"], opt.levels, initial_ge);

            // Write back
            write_file(subs_out / entry.path().filename(), j.dump(2));
            ++processed;
        }
    } else {
        std::cerr << "[WARN] subtilesets/ not found in input\n";
    }

    std::cout << "[OK] Processed " << processed << " files";
    if (skipped > 0) std::cout << " (" << skipped << " HLOD skipped)";
    std::cout << "\n";

    // ---- 3. Copy tileset.json --------------------------------------------
    fs::path tileset_in  = opt.input_dir / "tileset.json";
    fs::path tileset_out = opt.output_dir / "tileset.json";
    if (fs::exists(tileset_in)) {
        fs::copy_file(tileset_in, tileset_out, fs::copy_options::overwrite_existing);
        std::cout << "[OK] tileset.json copied\n";
    } else {
        std::cerr << "[WARN] tileset.json not found\n";
    }

    std::cout << "Done.\n";
    return 0;
}
