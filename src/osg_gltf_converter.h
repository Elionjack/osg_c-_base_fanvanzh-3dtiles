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
};

// Get full OSGB tile tree starting from a root file
osg_tree get_all_tree(std::string& file_name);

// Convert OSGB file to GLB buffer
bool osgb2glb_buf(std::string path, std::string& glb_buff, MeshInfo& mesh_info,
                  int node_type, bool enable_texture_compress = false,
                  bool enable_meshopt = false, bool enable_draco = false,
                  bool enable_unlit = true);

// Convert OSGB file to B3DM buffer (includes GLB inside)
bool osgb2b3dm_buf(std::string path, std::string& b3dm_buf, TileBox& tile_box,
                   int node_type, bool enable_texture_compress = false,
                   bool enable_meshopt = false, bool enable_draco = false,
                   bool enable_unlit = true);

// Process all tiles recursively
void do_tile_job(osg_tree& tree, std::string out_path, int max_lvl,
                 bool enable_texture_compress = false, bool enable_meshopt = false,
                 bool enable_draco = false, bool enable_unlit = true);

// Bounding box operations
void expend_box(TileBox& box, TileBox& box_new);
TileBox extend_tile_box(osg_tree& tree);
void calc_geometric_error(osg_tree& tree);

// JSON generation for tile trees
std::string encode_tile_json(osg_tree& tree, double x, double y);
std::string get_boundingBox(TileBox bbox);

// Global transformer access
coords::CoordinateTransformer* GetGlobalTransformer();
void SetGlobalTransformer(coords::CoordinateTransformer* t);
