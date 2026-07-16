#pragma once

#include <string>
#include <vector>
#include <map>
#include <osg/Node>
#include <osg/Geometry>
#include <nlohmann/json.hpp>
#include "utils.h"

// Forward declarations
namespace tinygltf {
    class Model;
    class Buffer;
}
namespace coords { class CoordinateTransformer; }
namespace osgb_converter { struct ConvertOptions; }

// Forward declaration for FlatTile
struct osg_tree;

// ============================================================
// FlatTile: flattened tile for parallel task pool
// ============================================================
struct FlatTile {
    std::string file_name;   // OSGB 文件路径
    std::string out_dir;     // 输出目录
    int type = 1;            // 对应 osg_tree::type
    osg_tree* tree = nullptr; // 指向 osg_tree 节点（用于回写 bbox）
};

struct MeshInfo {
    std::string name;
    std::vector<double> min;
    std::vector<double> max;
};

struct osg_tree {
    TileBox bbox;
    double geometricError = 0.0;
    std::string file_name;
    std::vector<osg_tree> sub_nodes;
    // type: 0=group, 1=PagedLOD nodes (default), 2=Other nodes
    int type = 1;
    // Cached node from Phase 1 — avoids redundant readNodeFiles in Phase 2
    osg::ref_ptr<osg::Node> cached_node;
};

// Get full OSGB tile tree starting from a root file
osg_tree get_all_tree(std::string& file_name);

// Core: convert a pre-loaded OSG Node to GLB buffer.
// Unlike osgb2glb_buf(), this does NOT call osgDB::readNodeFiles() —
// the caller provides the already-loaded node. Thread-safe for parallel use.
bool osgb2glb_buf_from_node(osg::Node* root, std::string parent_path,
                            std::string& glb_buff, MeshInfo& mesh_info,
                            int node_type, bool enable_texture_compress = false,
                            bool enable_meshopt = false, bool enable_draco = false,
                            bool enable_unlit = true,
                            double simplify_ratio = 0.5,
                            int draco_pos_bits = 11, int draco_normal_bits = 10,
                            int draco_uv_bits = 12, int ktx2_quality = 128);

// Convert OSGB file to GLB buffer (reads from disk via osgDB::readNodeFiles)
bool osgb2glb_buf(std::string path, std::string& glb_buff, MeshInfo& mesh_info,
                  int node_type, bool enable_texture_compress = false,
                  bool enable_meshopt = false, bool enable_draco = false,
                  bool enable_unlit = true,
                  double simplify_ratio = 0.5,
                  int draco_pos_bits = 11, int draco_normal_bits = 10,
                  int draco_uv_bits = 12, int ktx2_quality = 128);

// Convert OSGB file to B3DM buffer (includes GLB inside)
bool osgb2b3dm_buf(std::string path, std::string& b3dm_buf, TileBox& tile_box,
                   int node_type, bool enable_texture_compress = false,
                   bool enable_meshopt = false, bool enable_draco = false,
                   bool enable_unlit = true,
                   double simplify_ratio = 0.5,
                   int draco_pos_bits = 11, int draco_normal_bits = 10,
                   int draco_uv_bits = 12, int ktx2_quality = 128);

// Process all tiles recursively
void do_tile_job(osg_tree& tree, std::string out_path, int max_lvl,
                 bool enable_texture_compress = false, bool enable_meshopt = false,
                 bool enable_draco = false, bool enable_unlit = true,
                 double simplify_ratio = 0.5,
                 int draco_pos_bits = 11, int draco_normal_bits = 10,
                 int draco_uv_bits = 12, int ktx2_quality = 128);

// Bounding box operations
void expend_box(TileBox& box, TileBox& box_new);
TileBox extend_tile_box(osg_tree& tree);
void calc_geometric_error(osg_tree& tree);

// JSON generation for tile trees
std::string encode_tile_json(osg_tree& tree, double x, double y);
std::string get_boundingBox(TileBox bbox);

// ============================================================
// 3D Tiles 1.1 specific functions
// ============================================================

// Process all tiles recursively — outputs raw .glb (no B3DM wrapper)
void do_tile_job_1_1(osg_tree& tree, std::string out_path, int max_lvl,
                     bool enable_texture_compress = false, bool enable_meshopt = false,
                     bool enable_draco = false, bool enable_unlit = true,
                     double simplify_ratio = 0.5,
                     int draco_pos_bits = 11, int draco_normal_bits = 10,
                     int draco_uv_bits = 12, int ktx2_quality = 128);

// JSON generation for tile trees (3D Tiles 1.1 compatible)
// Uses 3DTILES_content_gltf extension and .glb URIs
std::string encode_tile_json_1_1(osg_tree& tree, double x, double y);

// Global transformer access
coords::CoordinateTransformer* GetGlobalTransformer();
void SetGlobalTransformer(coords::CoordinateTransformer* t);

// Convert a single tile from OSGB to GLB and write to disk.
// Designed to be called from parallel task pools.
// Populates tile->bbox with the geometry bounds.
bool convert_one_tile(const FlatTile& tile, const osgb_converter::ConvertOptions& opts);

// Convert a single tile using pre-loaded cached_node from Phase 1.
// Avoids redundant osgDB::readNodeFiles() and its global mutex.
// Falls back to disk read if cached_node is invalid.
bool convert_one_tile_from_cached(const FlatTile& tile,
                                   const osgb_converter::ConvertOptions& opts);

// Compute GLB buffer for a single tile without writing to disk.
// Does all the CPU work (InfoVisitor, GLB serialization, compression)
// but returns the buffer and output path instead of writing.
// Caller is responsible for bbox write-back and file I/O.
bool compute_tile_output(const FlatTile& tile,
                         const osgb_converter::ConvertOptions& opts,
                         std::string& glb_buf, MeshInfo& minfo,
                         std::string& out_file);

// Recursively collect all type>0 nodes from an osg_tree into a flat list.
// Used by Phase 2 of the parallel pipeline.
void collect_flat_tiles(osg_tree& tree, const std::string& out_dir,
                        std::vector<FlatTile>& out);

// ============================================================
// Quadtree HLOD structures
// ============================================================

// Grid cell in spatial grid (one per top-level tile directory)
struct GridCell {
    std::string stem;            // e.g. "Tile_-001_+050"
    int grid_x = 0;              // parsed grid X coordinate
    int grid_y = 0;              // parsed grid Y coordinate
    std::string coarsest_path;   // coarsest OSGB path in this cell
    TileBox bbox;                // bounding box
    double geometricError = 0.0; // geometric error
    std::string glb_uri;         // URI to existing GLB (e.g. "./Tile_-001_+050.glb")
};

// Spatial grid: grid_x → grid_y → GridCell
using SpatialGrid = std::map<int, std::map<int, GridCell>>;

// Quadtree node for HLOD hierarchy
struct QuadNode {
    int grid_x = 0;              // top-left grid coordinate
    int grid_y = 0;
    int grid_size = 0;           // size in grid cells (1, 2, 4, 8, ...)
    int level = 0;               // 0 = finest (leaf), higher = coarser
    std::string stem;            // tile stem (only for leaf nodes, e.g. "Tile_-001_+050")
    TileBox bbox;                // bounding box (union of children or from merge)
    double geometricError = 0.0;
    std::string glb_uri;         // output GLB URI (relative path)
    bool has_content = false;    // true = has geometry (content in tileset)
    std::vector<QuadNode> children; // 0-4 sub-quadtree nodes
    std::vector<std::string> leaf_stems;           // level=0: tile stems under this node
    std::vector<std::string> leaf_coarsest_paths;  // level=0: coarsest OSGB paths under this node
};

// ============================================================
// Root tile reconstruction (top-level merge)
// ============================================================

// Parse tile grid coordinates from tile name stem
// e.g. "Tile_-001_+050" → (-1, 50)
bool parse_tile_grid_coords(const std::string& stem, int& out_x, int& out_y);

// Build spatial grid from tile results (Phase 3 output)
SpatialGrid build_spatial_grid(
    const std::vector<std::string>& tile_stems,
    const std::vector<std::string>& coarsest_paths,
    const std::vector<TileBox>& bboxes,
    const std::vector<double>& coarsest_ges);

// Calculate simplification ratio for a given quadtree level
double calc_level_ratio(int level, double base_ratio);

// Build quadtree bottom-up from spatial grid.
// Returns the root node (highest level).
QuadNode build_quadtree(const SpatialGrid& grid);

// Collect all leaf OSGB paths under a quadtree node (recursive).
void collect_leaf_paths(const QuadNode& node, const SpatialGrid& grid,
                        std::vector<std::string>& paths);

// General merge function: merge multiple OSGB files into one GLB.
// Uses level to determine simplification ratio.
bool build_merged_glb(
    const std::vector<std::string>& osgb_paths,
    int quadtree_level,
    std::string& out_glb_buf,
    TileBox& out_bbox,
    bool enable_texture_compress,
    bool enable_meshopt,
    bool enable_draco,
    bool enable_unlit,
    int top_texture_max_size = 512,
    double simplify_ratio = 0.5,
    int draco_pos_bits = 11, int draco_normal_bits = 10,
    int draco_uv_bits = 12, int ktx2_quality = 128);

// Generate tileset JSON for a quadtree node hierarchy.
// Returns a nlohmann::json object representing the tile subtree.
// tile_jsons maps stem→json for embedding PagedLOD subtrees at leaves.
// When split_depth > 0, nodes at display_level == split_depth are
// written as external sub-tilesets and replaced by reference tiles.
// When externalize_pagedlod is true, level-0 PagedLOD subtrees are
// always externalized (regardless of display_level/split_depth).
nlohmann::json encode_quadtree_json(
    const QuadNode& node,
    const std::map<std::string, nlohmann::json>& tile_jsons,
    int current_display_level = 0,
    int root_level = 0,
    int split_depth = 0,
    const std::string& output_dir = "",
    bool externalize_pagedlod = false);

// Returns the coarsest-LOD file for a tile tree: the tree root
// (file without _Lxx suffix). Optionally outputs its geometricError.
std::string find_coarsest_node(const osg_tree& tree, double* out_ge = nullptr);

// Build a merged root-level GLB from multiple OSGB files.
// Loads each file, applies SVD coordinate correction, merges all
// geometry into one tinygltf::Model, and serializes to GLB.
// Returns the GLB buffer and combined bounding box via out parameters.
bool build_merged_root_glb(
    const std::vector<std::string>& coarsest_paths,
    std::string& out_glb_buf,
    TileBox& out_bbox,
    bool enable_texture_compress,
    bool enable_meshopt,
    bool enable_draco,
    bool enable_unlit,
    int top_texture_max_size = 512,
    double simplify_ratio = 0.5,
    int draco_pos_bits = 11, int draco_normal_bits = 10,
    int draco_uv_bits = 12, int ktx2_quality = 128);
