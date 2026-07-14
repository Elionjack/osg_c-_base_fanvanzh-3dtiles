#pragma once

#include <string>
#include <vector>
#include <osg/Node>
#include <osg/Geometry>
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
                            int draco_uv_bits = 12);

// Convert OSGB file to GLB buffer (reads from disk via osgDB::readNodeFiles)
bool osgb2glb_buf(std::string path, std::string& glb_buff, MeshInfo& mesh_info,
                  int node_type, bool enable_texture_compress = false,
                  bool enable_meshopt = false, bool enable_draco = false,
                  bool enable_unlit = true,
                  double simplify_ratio = 0.5,
                  int draco_pos_bits = 11, int draco_normal_bits = 10,
                  int draco_uv_bits = 12);

// Convert OSGB file to B3DM buffer (includes GLB inside)
bool osgb2b3dm_buf(std::string path, std::string& b3dm_buf, TileBox& tile_box,
                   int node_type, bool enable_texture_compress = false,
                   bool enable_meshopt = false, bool enable_draco = false,
                   bool enable_unlit = true,
                   double simplify_ratio = 0.5,
                   int draco_pos_bits = 11, int draco_normal_bits = 10,
                   int draco_uv_bits = 12);

// Process all tiles recursively
void do_tile_job(osg_tree& tree, std::string out_path, int max_lvl,
                 bool enable_texture_compress = false, bool enable_meshopt = false,
                 bool enable_draco = false, bool enable_unlit = true,
                 double simplify_ratio = 0.5,
                 int draco_pos_bits = 11, int draco_normal_bits = 10,
                 int draco_uv_bits = 12);

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
                     int draco_uv_bits = 12);

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
// Root tile reconstruction (top-level merge)
// ============================================================

// Find the file path of the coarsest-LOD node in a tile tree.
// Returns the file_name of the node with the highest _Lxx level number.
// Falls back to the tree's own file_name if no coarser children found.
std::string find_coarsest_node(const osg_tree& tree);

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
    int draco_uv_bits = 12);
