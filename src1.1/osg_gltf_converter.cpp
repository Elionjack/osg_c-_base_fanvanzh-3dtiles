#include "osg_gltf_converter.h"
#include "osgb_converter.h"
#include "coordinate_transformer.h"
#include "mesh_processor.h"

#include <osg/Material>
#include <osg/PagedLOD>
#include <osgDB/ReadFile>
#include <osgDB/ConvertUTF>
#include <osgUtil/SmoothingVisitor>
#include <Eigen/Eigen>

#include <set>
#include <map>
#include <algorithm>
#include <sstream>
#include <functional>
#include <cstdint>
#include <limits>
#include <cstdlib>
#include <cmath>
#include <climits>

#define TINYGLTF_IMPLEMENTATION
#include <tiny_gltf.h>

// stb_image_write used by tinygltf's TINYGLTF_IMPLEMENTATION and by mesh_processor
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

// stb_image_resize for root tile texture downsampling
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb_image_resize2.h>

// OSG plugin registration (needed for static linking on Linux/macOS)
#if defined(__unix__) || defined(__APPLE__)
#include <atomic>
#include <chrono>
#include <osgDB/Registry>
USE_OSGPLUGIN(osg)
USE_OSGPLUGIN(osg2)
USE_OSGPLUGIN(rgb)
USE_OSGPLUGIN(tga)
USE_OSGPLUGIN(jpeg)
USE_OSGPLUGIN(png)
USE_SERIALIZER_WRAPPER_LIBRARY(osg)
#endif

using namespace std;

#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif

// ============================================================
// Global transformer (replaces Rust-exposed global state)
// ============================================================
static coords::CoordinateTransformer* g_transformer = nullptr;

coords::CoordinateTransformer* GetGlobalTransformer() {
    return g_transformer;
}

void SetGlobalTransformer(coords::CoordinateTransformer* t) {
    g_transformer = t;
}

// ============================================================
// OSG path encoding helpers
// ============================================================
static std::string normalize_path(const char* path) {
    if (!path) return std::string();
#ifdef _WIN32
    std::string p(path);
    const std::string uncPrefix = R"(\\?\UNC\)";
    if (p.rfind(uncPrefix, 0) == 0) return R"(\\)" + p.substr(uncPrefix.length());
    const std::string longPrefix = R"(\\?\)";
    if (p.rfind(longPrefix, 0) == 0) return p.substr(longPrefix.length());
    return p;
#else
    return std::string(path);
#endif
}

static std::string osg_string(const char* path) {
#ifdef WIN32
    return osgDB::convertStringFromUTF8toCurrentCodePage(normalize_path(path));
#else
    return std::string(path);
#endif
}

static std::string utf8_string(const char* path) {
#ifdef WIN32
    return osgDB::convertStringFromCurrentCodePageToUTF8(path);
#else
    return std::string(path);
#endif
}

// ============================================================
// OSG Plugin logging
// ============================================================
static void log_osg_plugin_info() {
    fprintf(stderr, "\n=== OpenSceneGraph Plugin Loading ===\n");
    osgDB::Registry* registry = osgDB::Registry::instance();

    // OSG reads OSG_LIBRARY_PATH in Registry constructor, but that may have
    // run before we set the env var (e.g. during DLL static init). Force-add
    // any paths from the current OSG_LIBRARY_PATH env var to the search list.
    const char* osg_lib_path = getenv("OSG_LIBRARY_PATH");
    if (osg_lib_path && osg_lib_path[0]) {
        std::string env_paths(osg_lib_path);
        auto& libPaths = registry->getLibraryFilePathList();
        // Split by ';' (Windows) or ':' (Unix)
#ifdef _WIN32
        const char delim = ';';
#else
        const char delim = ':';
#endif
        size_t start = 0, end;
        while ((end = env_paths.find(delim, start)) != std::string::npos) {
            std::string one = env_paths.substr(start, end - start);
            if (!one.empty() && std::find(libPaths.begin(), libPaths.end(), one) == libPaths.end())
                libPaths.insert(libPaths.begin(), one);
            start = end + 1;
        }
        std::string last = env_paths.substr(start);
        if (!last.empty() && std::find(libPaths.begin(), libPaths.end(), last) == libPaths.end())
            libPaths.insert(libPaths.begin(), last);
    }

    fprintf(stderr, "OSG_LIBRARY_PATH: %s\n", osg_lib_path ? osg_lib_path : "NOT SET");
    const auto& libPaths = registry->getLibraryFilePathList();
    fprintf(stderr, "OSG Library Search Paths (%zu):\n", libPaths.size());
    for (size_t i = 0; i < libPaths.size(); ++i)
        fprintf(stderr, "  [%zu] %s\n", i, libPaths[i].c_str());
    fprintf(stderr, "=== End OSG Plugin Info ===\n\n");
}

// ============================================================
// InfoVisitor - traverse OSG node tree to collect geometry
// ============================================================
class InfoVisitor : public osg::NodeVisitor {
    std::string path;
    bool skip_correction;  // true for get_all_tree (only need sub_node_names)
public:
    InfoVisitor(std::string _path, bool loadAllType = false, bool _skip_correction = false)
        : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
        , path(_path), is_loadAllType(loadAllType), is_pagedlod(loadAllType)
        , skip_correction(_skip_correction) {}

    ~InfoVisitor() {}

    void apply(osg::Geometry& geometry) {
        if (geometry.getVertexArray() == nullptr
            || geometry.getVertexArray()->getDataSize() == 0U
            || geometry.getNumPrimitiveSets() == 0U)
            return;

        if (is_pagedlod)
            geometry_array.push_back(&geometry);
        else
            other_geometry_array.push_back(&geometry);

        // Coordinate correction using global transformer
        // Skip in get_all_tree() — only need sub_node_names, geometry is discarded.
        coords::CoordinateTransformer* transformer = GetGlobalTransformer();
        bool needs_transform = !skip_correction && transformer && transformer->HasGeoReference();

        if (needs_transform) {
            osg::Vec3Array* vertexArr = (osg::Vec3Array*)geometry.getVertexArray();

            // 1. Compute bounding box
            glm::dvec3 Min(DBL_MAX), Max(-DBL_MAX);
            for (int vi = 0; vi < (int)vertexArr->size(); vi++) {
                osg::Vec3d v = vertexArr->at(vi);
                glm::dvec3 vert(v.x(), v.y(), v.z());
                Min = glm::min(vert, Min);
                Max = glm::max(vert, Max);
            }

            // 2. Correct 8 corners of bounding box
            auto Correction = [&](glm::dvec3 pt) { return transformer->ToLocalENU(pt); };
            vector<glm::dvec4> Orig(8), Corr(8);
            Orig[0] = glm::dvec4(Min.x, Min.y, Min.z, 1);
            Orig[1] = glm::dvec4(Max.x, Min.y, Min.z, 1);
            Orig[2] = glm::dvec4(Min.x, Max.y, Min.z, 1);
            Orig[3] = glm::dvec4(Min.x, Min.y, Max.z, 1);
            Orig[4] = glm::dvec4(Max.x, Max.y, Min.z, 1);
            Orig[5] = glm::dvec4(Min.x, Max.y, Max.z, 1);
            Orig[6] = glm::dvec4(Max.x, Min.y, Max.z, 1);
            Orig[7] = glm::dvec4(Max.x, Max.y, Max.z, 1);
            for (int i = 0; i < 8; i++)
                Corr[i] = glm::dvec4(Correction(glm::dvec3(Orig[i])), 1);

            // 3. SVD least-squares to find best-fit transform
            Eigen::MatrixXd A(8, 4), B(8, 4);
            for (int r = 0; r < 8; r++) {
                for (int c = 0; c < 4; c++) {
                    A(r, c) = (&Orig[r].x)[c];
                    B(r, c) = (&Corr[r].x)[c];
                }
            }
            Eigen::BDCSVD<Eigen::MatrixXd> SVD(A, Eigen::ComputeThinU | Eigen::ComputeThinV);
            Eigen::MatrixXd X = SVD.solve(B);

            // 4. Apply correction matrix to all vertices
            glm::dmat4 Transform(
                X(0,0), X(0,1), X(0,2), X(0,3),
                X(1,0), X(1,1), X(1,2), X(1,3),
                X(2,0), X(2,1), X(2,2), X(2,3),
                X(3,0), X(3,1), X(3,2), X(3,3));

            for (int vi = 0; vi < (int)vertexArr->size(); vi++) {
                osg::Vec3d v = vertexArr->at(vi);
                glm::dvec4 v4 = Transform * glm::dvec4(v.x(), v.y(), v.z(), 1);
                vertexArr->at(vi) = osg::Vec3d(v4.x, v4.y, v4.z);
            }
        }

        // Collect texture
        if (auto ss = geometry.getStateSet()) {
            osg::Texture* tex = dynamic_cast<osg::Texture*>(ss->getTextureAttribute(0, osg::StateAttribute::TEXTURE));
            if (tex) {
                if (is_pagedlod)
                    texture_array.insert(tex);
                else
                    other_texture_array.insert(tex);
                texture_map[&geometry] = tex;
            }
        }
    }

    void apply(osg::PagedLOD& node) {
        int n = node.getNumFileNames();
        for (size_t i = 1; i < (size_t)n; i++) {
            std::string file_name = path + "/" + node.getFileName(i);
            sub_node_names.push_back(file_name);
        }
        if (!is_loadAllType) is_pagedlod = true;
        traverse(node);
        if (!is_loadAllType) is_pagedlod = false;
    }

public:
    // PagedLOD geometry
    std::vector<osg::Geometry*> geometry_array;
    std::set<osg::Texture*> texture_array;
    std::map<osg::Geometry*, osg::Texture*> texture_map;
    std::vector<std::string> sub_node_names;
    bool is_loadAllType;
    bool is_pagedlod;
    // Other geometry (non-PagedLOD)
    std::vector<osg::Geometry*> other_geometry_array;
    std::set<osg::Texture*> other_texture_array;
};

// ============================================================
// Tree traversal
// ============================================================
osg_tree get_all_tree(std::string& file_name) {
    osg_tree root_tile;
    vector<string> fileNames = { file_name };

    static bool logged = false;
    if (!logged) { log_osg_plugin_info(); logged = true; }

    InfoVisitor infoVisitor(get_parent(file_name), false, /*skip_correction=*/true);
    {
        osg::ref_ptr<osg::Node> root = osgDB::readNodeFiles(fileNames);
        if (!root) {
            std::string name = utf8_string(file_name.c_str());
            LOG_E("read node files [%s] fail!", name.c_str());
            return root_tile;
        }
        root_tile.file_name = file_name;
        root_tile.type = 1;
        root->accept(infoVisitor);
    }
    // NOTE: cached_node is NOT set here. Phase 1 only builds the file tree
    // structure (sub_node_names).  Phase 2 loads each tile from disk on demand
    // and frees it after conversion, keeping memory bounded.

    for (auto& i : infoVisitor.sub_node_names) {
        osg_tree tree = get_all_tree(i);
        if (!tree.file_name.empty()) {
            if (tree.type == 0) {
                for (auto& node : tree.sub_nodes)
                    root_tile.sub_nodes.push_back(node);
            } else {
                root_tile.sub_nodes.push_back(tree);
            }
        }
    }

    // If node has both PagedLOD and Other geometry, wrap in a group node
    if (!infoVisitor.other_geometry_array.empty() && !infoVisitor.geometry_array.empty()) {
        osg_tree new_root_tile;
        new_root_tile.type = 0;
        new_root_tile.file_name = file_name;
        osg_tree tile;
        tile.type = 2;
        tile.file_name = file_name;
        new_root_tile.sub_nodes.push_back(root_tile);
        new_root_tile.sub_nodes.push_back(tile);
        root_tile = new_root_tile;
    }
    return root_tile;
}

// ============================================================
// Bounding box helpers
// ============================================================
void expend_box(TileBox& box, TileBox& box_new) {
    if (box_new.max.empty() || box_new.min.empty()) return;
    if (box.max.empty()) { box.max = box_new.max; box.min = box_new.min; return; }
    for (int i = 0; i < 3; i++) {
        if (box.min[i] > box_new.min[i]) box.min[i] = box_new.min[i];
        if (box.max[i] < box_new.max[i]) box.max[i] = box_new.max[i];
    }
}

TileBox extend_tile_box(osg_tree& tree) {
    TileBox box = tree.bbox;
    for (auto& i : tree.sub_nodes) {
        TileBox sub = extend_tile_box(i);
        expend_box(box, sub);
    }
    tree.bbox = box;
    return box;
}

void calc_geometric_error(osg_tree& tree) {
    for (auto& i : tree.sub_nodes) calc_geometric_error(i);
    if (tree.sub_nodes.empty()) {
        //Leaf tile: must be 0.0 per 3D Tiles spec
        tree.geometricError = 0.0;
    } else {
        //Check if direct parent of leaves
        bool all_children_are_leaves = true;
        for (auto& s : tree.sub_nodes) {
            if (!s.sub_nodes.empty()) { all_children_are_leaves = false; break; }
        }
        double max_sub = 0.0;
        if (all_children_are_leaves) {
            //Direct parent of leaves: physical size * 0.1
            for (auto& s : tree.sub_nodes)
                max_sub = std::max(max_sub, get_geometric_error(s.bbox) * 0.1);
        } else {
            //Upper parent: max child ge * 2.0
            for (auto& s : tree.sub_nodes)
                max_sub = std::max(max_sub, s.geometricError);
            max_sub = max_sub * 2.0;
        }
        tree.geometricError = max_sub;
    }
}

// ============================================================
// Index component type selection
// ============================================================
static int pick_index_component_type(uint32_t max_index) {
    if (max_index <= 255) return TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;
    if (max_index <= 65535) return TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT;
    return TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT;
}

// ============================================================
// glTF write helpers
// ============================================================
struct OsgBuildState {
    tinygltf::Buffer* buffer;
    tinygltf::Model* model;
    osg::Vec3f point_max;
    osg::Vec3f point_min;
    int draw_array_first;
    int draw_array_count;
};

static void expand_bbox3d(osg::Vec3f& pmax, osg::Vec3f& pmin, osg::Vec3f p) {
    pmax.x() = std::max(p.x(), pmax.x()); pmin.x() = std::min(p.x(), pmin.x());
    pmax.y() = std::max(p.y(), pmax.y()); pmin.y() = std::min(p.y(), pmin.y());
    pmax.z() = std::max(p.z(), pmax.z()); pmin.z() = std::min(p.z(), pmin.z());
}

static void expand_bbox2d(osg::Vec2f& pmax, osg::Vec2f& pmin, osg::Vec2f p) {
    pmax.x() = std::max(p.x(), pmax.x()); pmin.x() = std::min(p.x(), pmin.x());
    pmax.y() = std::max(p.y(), pmax.y()); pmin.y() = std::min(p.y(), pmin.y());
}

// Write index buffer and create accessor + buffer view
static int write_index_vector(const std::vector<uint32_t>& indices, OsgBuildState* osgState,
                              bool dracoCompressed) {
    if (indices.empty()) return -1;

    uint32_t max_idx = 0, min_idx = UINT32_MAX;
    for (auto idx : indices) {
        max_idx = std::max(max_idx, idx);
        min_idx = std::min(min_idx, idx);
    }

    int compType = pick_index_component_type(max_idx);
    if (dracoCompressed) {
        tinygltf::Accessor acc;
        acc.bufferView = -1;
        acc.type = TINYGLTF_TYPE_SCALAR;
        acc.componentType = compType;
        acc.count = (int)indices.size();
        acc.maxValues = {(double)max_idx};
        acc.minValues = {(double)min_idx};
        int accIdx = (int)osgState->model->accessors.size();
        osgState->model->accessors.push_back(acc);
        return accIdx;
    }

    unsigned bufStart = (unsigned)osgState->buffer->data.size();
    switch (compType) {
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
            for (auto idx : indices) put_val(osgState->buffer->data, (uint8_t)idx); break;
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
            for (auto idx : indices) put_val(osgState->buffer->data, (uint16_t)idx); break;
        default:
            for (auto idx : indices) put_val(osgState->buffer->data, (uint32_t)idx); break;
    }
    alignment_buffer(osgState->buffer->data);

    tinygltf::Accessor acc;
    acc.bufferView = (int)osgState->model->bufferViews.size();
    acc.type = TINYGLTF_TYPE_SCALAR;
    acc.componentType = compType;
    acc.count = (int)indices.size();
    acc.maxValues = {(double)max_idx};
    acc.minValues = {(double)min_idx};
    int accIdx = (int)osgState->model->accessors.size();
    osgState->model->accessors.push_back(acc);

    tinygltf::BufferView bfv;
    bfv.buffer = 0;
    bfv.target = TINYGLTF_TARGET_ELEMENT_ARRAY_BUFFER;
    bfv.byteOffset = (int)bufStart;
    bfv.byteLength = (int)(osgState->buffer->data.size() - bufStart);
    osgState->model->bufferViews.push_back(bfv);
    return accIdx;
}

// Quad/quad-strip triangulation
static bool triangulate_quads(const std::vector<uint32_t>& indices, GLenum mode,
                              std::vector<uint32_t>& out) {
    out.clear();
    if (mode == GL_QUADS) {
        if (indices.size() < 4 || indices.size() % 4 != 0) return false;
        size_t nq = indices.size() / 4;
        out.reserve(nq * 6);
        for (size_t q = 0; q < nq; q++) {
            size_t b = q * 4;
            out.insert(out.end(), {indices[b], indices[b+1], indices[b+2],
                                   indices[b], indices[b+2], indices[b+3]});
        }
        return !out.empty();
    }
    if (mode == GL_QUAD_STRIP) {
        if (indices.size() < 4 || indices.size() % 2 != 0) return false;
        size_t np = indices.size() / 2;
        out.reserve((np - 1) * 6);
        for (size_t i = 0; i + 1 < np; i++) {
            size_t b = i * 2;
            if (b + 3 >= indices.size()) break;
            out.insert(out.end(), {indices[b], indices[b+1], indices[b+2],
                                   indices[b+1], indices[b+3], indices[b+2]});
        }
        return !out.empty();
    }
    return false;
}

// Write vec3 array (positions or normals)
static void write_vec3_array(osg::Vec3Array* v3f, OsgBuildState* osgState,
                             osg::Vec3f& pmax, osg::Vec3f& pmin, bool isNormal = false) {
    int vs = 0, ve = (int)v3f->size();
    if (osgState->draw_array_first >= 0) {
        vs = osgState->draw_array_first;
        ve = osgState->draw_array_count + vs;
    }
    unsigned bufStart = (unsigned)osgState->buffer->data.size();
    for (int vi = vs; vi < ve; vi++) {
        osg::Vec3f p = v3f->at(vi);
        if (isNormal) {
            float len = p.length();
            if (len < 0.0001f) p.set(0, 0, 1);
            else if (std::abs(len - 1.0f) > 0.0001f) p.normalize();
        }
        put_val(osgState->buffer->data, p.x());
        put_val(osgState->buffer->data, p.y());
        put_val(osgState->buffer->data, p.z());
        expand_bbox3d(pmax, pmin, p);
    }
    alignment_buffer(osgState->buffer->data);

    tinygltf::Accessor acc;
    acc.bufferView = (int)osgState->model->bufferViews.size();
    acc.count = ve - vs;
    acc.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
    acc.type = TINYGLTF_TYPE_VEC3;
    acc.maxValues = {(double)pmax.x(), (double)pmax.y(), (double)pmax.z()};
    acc.minValues = {(double)pmin.x(), (double)pmin.y(), (double)pmin.z()};
    osgState->model->accessors.push_back(acc);

    tinygltf::BufferView bfv;
    bfv.buffer = 0;
    bfv.target = TINYGLTF_TARGET_ARRAY_BUFFER;
    bfv.byteOffset = (int)bufStart;
    bfv.byteLength = (int)(osgState->buffer->data.size() - bufStart);
    osgState->model->bufferViews.push_back(bfv);
}

// Write vec2 array (texcoords)
static void write_vec2_array(osg::Vec2Array* v2f, OsgBuildState* osgState) {
    int vs = 0, ve = (int)v2f->size();
    if (osgState->draw_array_first >= 0) {
        vs = osgState->draw_array_first;
        ve = osgState->draw_array_count + vs;
    }
    osg::Vec2f pmax(-1e38f, -1e38f), pmin(1e38f, 1e38f);
    unsigned bufStart = (unsigned)osgState->buffer->data.size();
    for (int vi = vs; vi < ve; vi++) {
        osg::Vec2f p = v2f->at(vi);
        put_val(osgState->buffer->data, p.x());
        put_val(osgState->buffer->data, p.y());
        expand_bbox2d(pmax, pmin, p);
    }
    alignment_buffer(osgState->buffer->data);

    tinygltf::Accessor acc;
    acc.bufferView = (int)osgState->model->bufferViews.size();
    acc.count = ve - vs;
    acc.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
    acc.type = TINYGLTF_TYPE_VEC2;
    acc.maxValues = {(double)pmax.x(), (double)pmax.y()};
    acc.minValues = {(double)pmin.x(), (double)pmin.y()};
    osgState->model->accessors.push_back(acc);

    tinygltf::BufferView bfv;
    bfv.buffer = 0;
    bfv.target = TINYGLTF_TARGET_ARRAY_BUFFER;
    bfv.byteOffset = (int)bufStart;
    bfv.byteLength = (int)(osgState->buffer->data.size() - bufStart);
    osgState->model->bufferViews.push_back(bfv);
}

struct DracoState {
    bool compressed = false;
    int bufferView = -1;
    int posId = -1, normId = -1, texId = -1, batchId = -1;
};

// Per-primitive state for EXT_meshopt_compression (analogous to DracoState)
struct MeshoptState {
    bool compressed = false;
    struct StreamInfo {
        int bufferView = -1;     // glTF bufferView index for this compressed stream
        int count = 0;           // number of elements (vertices or indices)
        std::string mode;        // "ATTRIBUTES" or "TRIANGLES"
        std::string filter;      // "NONE" or "OCTAHEDRAL"
        int byteStride = 0;      // stride of uncompressed data (0 for TRIANGLES mode)
    };
    std::vector<StreamInfo> streams;
    // Which stream belongs to which attribute (-1 = absent)
    int posStreamIdx = -1;
    int normStreamIdx = -1;
    int uvStreamIdx = -1;
    int idxStreamIdx = -1;
};

// Write a single primitive set
static void write_primitive(osg::Geometry* g, osg::PrimitiveSet* ps,
                            OsgBuildState* osgState, int* pmtVertex, int* pmtNormal, int* pmtTexcd,
                            DracoState* dracoState, MeshoptState* meshoptState) {
    tinygltf::Primitive prim;
    prim.indices = (int)osgState->model->accessors.size();
    osgState->draw_array_first = -1;

    const GLenum gl_mode = ps->getMode();
    const bool needs_tri = (gl_mode == GL_QUADS || gl_mode == GL_QUAD_STRIP);
    std::vector<uint32_t> tri_indices;

    // Write indices
    osg::PrimitiveSet::Type t = ps->getType();
    switch (t) {
        case osg::PrimitiveSet::DrawElementsUBytePrimitiveType: {
            auto* de = static_cast<const osg::DrawElementsUByte*>(ps);
            // Meshopt-compressed indices (entire buffer already compressed)
            if (meshoptState && meshoptState->compressed && meshoptState->idxStreamIdx >= 0 && !needs_tri) {
                auto& idxS = meshoptState->streams[meshoptState->idxStreamIdx];
                tinygltf::Accessor acc;
                acc.bufferView = idxS.bufferView;
                acc.type = TINYGLTF_TYPE_SCALAR;
                acc.componentType = TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT;
                acc.count = idxS.count;
                prim.indices = (int)osgState->model->accessors.size();
                osgState->model->accessors.push_back(acc);
            } else if (needs_tri) {
                std::vector<uint32_t> src; src.reserve(de->size());
                for (unsigned m = 0; m < de->size(); m++) src.push_back(de->at(m));
                if (triangulate_quads(src, gl_mode, tri_indices))
                    prim.indices = write_index_vector(tri_indices, osgState, dracoState->compressed);
            } else {
                if (dracoState->compressed) {
                    tinygltf::Accessor acc;
                    acc.bufferView = -1; acc.type = TINYGLTF_TYPE_SCALAR;
                    acc.componentType = TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;
                    acc.count = (int)de->size();
                    prim.indices = (int)osgState->model->accessors.size();
                    osgState->model->accessors.push_back(acc);
                } else {
                    std::vector<uint32_t> idx; idx.reserve(de->size());
                    for (unsigned m = 0; m < de->size(); m++) idx.push_back(de->at(m));
                    prim.indices = write_index_vector(idx, osgState, false);
                }
            }
            break;
        }
        case osg::PrimitiveSet::DrawElementsUShortPrimitiveType: {
            auto* de = static_cast<const osg::DrawElementsUShort*>(ps);
            // Meshopt-compressed indices
            if (meshoptState && meshoptState->compressed && meshoptState->idxStreamIdx >= 0 && !needs_tri) {
                auto& idxS = meshoptState->streams[meshoptState->idxStreamIdx];
                tinygltf::Accessor acc;
                acc.bufferView = idxS.bufferView;
                acc.type = TINYGLTF_TYPE_SCALAR;
                acc.componentType = TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT;
                acc.count = idxS.count;
                prim.indices = (int)osgState->model->accessors.size();
                osgState->model->accessors.push_back(acc);
            } else if (needs_tri) {
                std::vector<uint32_t> src; src.reserve(de->size());
                for (unsigned m = 0; m < de->size(); m++) src.push_back(de->at(m));
                if (triangulate_quads(src, gl_mode, tri_indices))
                    prim.indices = write_index_vector(tri_indices, osgState, dracoState->compressed);
            } else {
                if (dracoState->compressed) {
                    tinygltf::Accessor acc;
                    acc.bufferView = -1; acc.type = TINYGLTF_TYPE_SCALAR;
                    acc.componentType = TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT;
                    acc.count = (int)de->size();
                    prim.indices = (int)osgState->model->accessors.size();
                    osgState->model->accessors.push_back(acc);
                } else {
                    std::vector<uint32_t> idx; idx.reserve(de->size());
                    for (unsigned m = 0; m < de->size(); m++) idx.push_back(de->at(m));
                    prim.indices = write_index_vector(idx, osgState, false);
                }
            }
            break;
        }
        case osg::PrimitiveSet::DrawElementsUIntPrimitiveType: {
            auto* de = static_cast<const osg::DrawElementsUInt*>(ps);
            // Meshopt-compressed indices
            if (meshoptState && meshoptState->compressed && meshoptState->idxStreamIdx >= 0 && !needs_tri) {
                auto& idxS = meshoptState->streams[meshoptState->idxStreamIdx];
                tinygltf::Accessor acc;
                acc.bufferView = idxS.bufferView;
                acc.type = TINYGLTF_TYPE_SCALAR;
                acc.componentType = TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT;
                acc.count = idxS.count;
                prim.indices = (int)osgState->model->accessors.size();
                osgState->model->accessors.push_back(acc);
            } else if (needs_tri) {
                std::vector<uint32_t> src; src.reserve(de->size());
                for (unsigned m = 0; m < de->size(); m++) src.push_back(de->at(m));
                if (triangulate_quads(src, gl_mode, tri_indices))
                    prim.indices = write_index_vector(tri_indices, osgState, dracoState->compressed);
            } else {
                if (dracoState->compressed) {
                    tinygltf::Accessor acc;
                    acc.bufferView = -1; acc.type = TINYGLTF_TYPE_SCALAR;
                    acc.componentType = TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT;
                    acc.count = (int)de->size();
                    prim.indices = (int)osgState->model->accessors.size();
                    osgState->model->accessors.push_back(acc);
                } else {
                    std::vector<uint32_t> idx; idx.reserve(de->size());
                    for (unsigned m = 0; m < de->size(); m++) idx.push_back(de->at(m));
                    prim.indices = write_index_vector(idx, osgState, false);
                }
            }
            break;
        }
        case osg::PrimitiveSet::DrawArraysPrimitiveType: {
            prim.indices = -1;
            auto* da = dynamic_cast<osg::DrawArrays*>(ps);
            osgState->draw_array_first = da->getFirst();
            osgState->draw_array_count = da->getCount();
            if (needs_tri && da->getCount() > 0) {
                std::vector<uint32_t> src; src.reserve(da->getCount());
                for (int i = 0; i < da->getCount(); i++) src.push_back(i);
                if (triangulate_quads(src, gl_mode, tri_indices))
                    prim.indices = write_index_vector(tri_indices, osgState, dracoState->compressed);
            }
            break;
        }
        default:
            LOG_E("unsupported PrimitiveSet type: %d", (int)t);
            return;
    }

    // Position
    auto* varr = (osg::Vec3Array*)g->getVertexArray();
    if (*pmtVertex > -1 && osgState->draw_array_first == -1) {
        prim.attributes["POSITION"] = *pmtVertex;
    } else {
        // Meshopt-compressed position (bufferView already contains compressed data)
        if (meshoptState && meshoptState->compressed && meshoptState->posStreamIdx >= 0) {
            auto& posS = meshoptState->streams[meshoptState->posStreamIdx];
            tinygltf::Accessor acc;
            acc.bufferView = posS.bufferView;
            acc.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
            acc.count = posS.count;
            acc.type = TINYGLTF_TYPE_VEC3;
            osg::Vec3f pmax(-1e38f,-1e38f,-1e38f), pmin(1e38f,1e38f,1e38f);
            int vs = (osgState->draw_array_first >= 0) ? osgState->draw_array_first : 0;
            int ve = (osgState->draw_array_first >= 0) ? (osgState->draw_array_count + vs) : (int)varr->size();
            for (int vi = vs; vi < ve; vi++) expand_bbox3d(pmax, pmin, varr->at(vi));
            acc.minValues = {(double)pmin.x(), (double)pmin.y(), (double)pmin.z()};
            acc.maxValues = {(double)pmax.x(), (double)pmax.y(), (double)pmax.z()};
            int accIdx = (int)osgState->model->accessors.size();
            osgState->model->accessors.push_back(acc);
            prim.attributes["POSITION"] = accIdx;
            if (*pmtVertex == -1 && osgState->draw_array_first == -1) *pmtVertex = accIdx;
            expand_bbox3d(osgState->point_max, osgState->point_min, pmax);
            expand_bbox3d(osgState->point_max, osgState->point_min, pmin);
        } else if (dracoState->compressed) {
            tinygltf::Accessor acc;
            acc.bufferView = -1;
            acc.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
            acc.count = (osgState->draw_array_first >= 0) ? osgState->draw_array_count : (int)varr->size();
            acc.type = TINYGLTF_TYPE_VEC3;
            osg::Vec3f pmax(-1e38f,-1e38f,-1e38f), pmin(1e38f,1e38f,1e38f);
            int vs = (osgState->draw_array_first >= 0) ? osgState->draw_array_first : 0;
            int ve = (osgState->draw_array_first >= 0) ? (osgState->draw_array_count + vs) : (int)varr->size();
            for (int vi = vs; vi < ve; vi++) expand_bbox3d(pmax, pmin, varr->at(vi));
            acc.minValues = {(double)pmin.x(), (double)pmin.y(), (double)pmin.z()};
            acc.maxValues = {(double)pmax.x(), (double)pmax.y(), (double)pmax.z()};
            int accIdx = (int)osgState->model->accessors.size();
            osgState->model->accessors.push_back(acc);
            prim.attributes["POSITION"] = accIdx;
            if (*pmtVertex == -1 && osgState->draw_array_first == -1) *pmtVertex = accIdx;
            expand_bbox3d(osgState->point_max, osgState->point_min, pmax);
            expand_bbox3d(osgState->point_max, osgState->point_min, pmin);
        } else {
            osg::Vec3f pmax(-1e38f,-1e38f,-1e38f), pmin(1e38f,1e38f,1e38f);
            prim.attributes["POSITION"] = (int)osgState->model->accessors.size();
            if (*pmtVertex == -1 && osgState->draw_array_first == -1)
                *pmtVertex = (int)osgState->model->accessors.size();
            write_vec3_array(varr, osgState, pmax, pmin);
            expand_bbox3d(osgState->point_max, osgState->point_min, pmax);
            expand_bbox3d(osgState->point_max, osgState->point_min, pmin);
        }
    }

    // Normal
    auto* narr = (osg::Vec3Array*)g->getNormalArray();
    if (narr) {
        if (*pmtNormal > -1 && osgState->draw_array_first == -1) {
            prim.attributes["NORMAL"] = *pmtNormal;
        } else {
            // Meshopt-compressed normal
            if (meshoptState && meshoptState->compressed && meshoptState->normStreamIdx >= 0) {
                auto& normS = meshoptState->streams[meshoptState->normStreamIdx];
                tinygltf::Accessor acc;
                acc.bufferView = normS.bufferView;
                acc.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
                acc.count = normS.count;
                acc.type = TINYGLTF_TYPE_VEC3;
                int accIdx = (int)osgState->model->accessors.size();
                osgState->model->accessors.push_back(acc);
                prim.attributes["NORMAL"] = accIdx;
                if (*pmtNormal == -1 && osgState->draw_array_first == -1) *pmtNormal = accIdx;
            } else if (dracoState->compressed) {
                tinygltf::Accessor acc;
                acc.bufferView = -1;
                acc.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
                acc.count = (osgState->draw_array_first >= 0) ? osgState->draw_array_count : (int)narr->size();
                acc.type = TINYGLTF_TYPE_VEC3;
                int accIdx = (int)osgState->model->accessors.size();
                osgState->model->accessors.push_back(acc);
                prim.attributes["NORMAL"] = accIdx;
                if (*pmtNormal == -1 && osgState->draw_array_first == -1) *pmtNormal = accIdx;
            } else {
                osg::Vec3f pmax(-1e38f,-1e38f,-1e38f), pmin(1e38f,1e38f,1e38f);
                prim.attributes["NORMAL"] = (int)osgState->model->accessors.size();
                if (*pmtNormal == -1 && osgState->draw_array_first == -1)
                    *pmtNormal = (int)osgState->model->accessors.size();
                write_vec3_array(narr, osgState, pmax, pmin, true);
            }
        }
    }

    // Texcoord
    auto* tarr = (osg::Vec2Array*)g->getTexCoordArray(0);
    if (tarr) {
        if (*pmtTexcd > -1 && osgState->draw_array_first == -1) {
            prim.attributes["TEXCOORD_0"] = *pmtTexcd;
        } else {
            // Meshopt-compressed texcoord
            if (meshoptState && meshoptState->compressed && meshoptState->uvStreamIdx >= 0) {
                auto& uvS = meshoptState->streams[meshoptState->uvStreamIdx];
                tinygltf::Accessor acc;
                acc.bufferView = uvS.bufferView;
                acc.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
                acc.count = uvS.count;
                acc.type = TINYGLTF_TYPE_VEC2;
                int accIdx = (int)osgState->model->accessors.size();
                osgState->model->accessors.push_back(acc);
                prim.attributes["TEXCOORD_0"] = accIdx;
                if (*pmtTexcd == -1 && osgState->draw_array_first == -1) *pmtTexcd = accIdx;
            } else if (dracoState->compressed) {
                tinygltf::Accessor acc;
                acc.bufferView = -1;
                acc.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
                acc.count = (osgState->draw_array_first >= 0) ? osgState->draw_array_count : (int)tarr->size();
                acc.type = TINYGLTF_TYPE_VEC2;
                int accIdx = (int)osgState->model->accessors.size();
                osgState->model->accessors.push_back(acc);
                prim.attributes["TEXCOORD_0"] = accIdx;
                if (*pmtTexcd == -1 && osgState->draw_array_first == -1) *pmtTexcd = accIdx;
            } else {
                prim.attributes["TEXCOORD_0"] = (int)osgState->model->accessors.size();
                if (*pmtTexcd == -1 && osgState->draw_array_first == -1)
                    *pmtTexcd = (int)osgState->model->accessors.size();
                write_vec2_array(tarr, osgState);
            }
        }
    }

    prim.material = -1;

    // Mode mapping
    switch (ps->getMode()) {
        case GL_POINTS:         prim.mode = TINYGLTF_MODE_POINTS; break;
        case GL_LINES:          prim.mode = TINYGLTF_MODE_LINE; break;
        case GL_LINE_LOOP:      prim.mode = TINYGLTF_MODE_LINE_LOOP; break;
        case GL_LINE_STRIP:     prim.mode = TINYGLTF_MODE_LINE_STRIP; break;
        case GL_TRIANGLES:      prim.mode = TINYGLTF_MODE_TRIANGLES; break;
        case GL_TRIANGLE_STRIP: prim.mode = TINYGLTF_MODE_TRIANGLE_STRIP; break;
        case GL_TRIANGLE_FAN:   prim.mode = TINYGLTF_MODE_TRIANGLE_FAN; break;
        case GL_QUADS:
        case GL_QUAD_STRIP:     prim.mode = TINYGLTF_MODE_TRIANGLES; break;
        default:
            LOG_E("unsupported primitive mode: %d", (int)ps->getMode());
            return;
    }
    osgState->model->meshes.back().primitives.push_back(prim);

    // Draco extension
    if (dracoState->compressed) {
        auto& bp = osgState->model->meshes.back().primitives.back();
        tinygltf::Value::Object dracoExt;
        dracoExt["bufferView"] = tinygltf::Value(dracoState->bufferView);
        tinygltf::Value::Object attr;
        if (dracoState->posId != -1) attr["POSITION"] = tinygltf::Value(dracoState->posId);
        if (dracoState->normId != -1) attr["NORMAL"] = tinygltf::Value(dracoState->normId);
        if (dracoState->texId != -1) attr["TEXCOORD_0"] = tinygltf::Value(dracoState->texId);
        if (dracoState->batchId != -1) attr["_BATCHID"] = tinygltf::Value(dracoState->batchId);
        dracoExt["attributes"] = tinygltf::Value(attr);
        bp.extensions["KHR_draco_mesh_compression"] = tinygltf::Value(dracoExt);
    }

    // EXT_meshopt_compression extension (array of per-stream objects)
    if (meshoptState && meshoptState->compressed && !meshoptState->streams.empty()) {
        tinygltf::Value::Array meshoptExts;
        for (auto& si : meshoptState->streams) {
            tinygltf::Value::Object obj;
            obj["bufferView"] = tinygltf::Value(si.bufferView);
            obj["count"] = tinygltf::Value(si.count);
            obj["mode"] = tinygltf::Value(si.mode);
            obj["filter"] = tinygltf::Value(si.filter);
            if (si.byteStride > 0)
                obj["byteStride"] = tinygltf::Value(si.byteStride);
            meshoptExts.push_back(tinygltf::Value(obj));
        }
        prim.extensions["EXT_meshopt_compression"] = tinygltf::Value(meshoptExts);
    }
}

static tinygltf::Material make_default_material(double r = 1.0, double g = 1.0, double b = 1.0) {
    tinygltf::Material m;
    m.name = "default";
    m.pbrMetallicRoughness.baseColorFactor = {r, g, b, 1.0};
    m.pbrMetallicRoughness.metallicFactor = 0.0;
    m.pbrMetallicRoughness.roughnessFactor = 1.0;
    return m;
}

// ============================================================
// Main conversion: OSGB geometry → GLB buffer
// ============================================================
static void write_osgGeometry(osg::Geometry* g, OsgBuildState* osgState,
                              bool enable_simplify, bool enable_draco,
                              double simplify_ratio = 0.5,
                              int draco_pos_bits = 11, int draco_normal_bits = 10,
                              int draco_uv_bits = 12) {
    // ================================================================
    // Step 1: Mesh simplification (meshoptimizer)
    // ================================================================
    MeshoptState meshoptState;
    if (enable_simplify) {
        SimplificationParams sParams;
        sParams.enable_simplification = true;
        sParams.target_ratio = (float)simplify_ratio;
        ::simplify_mesh_geometry(g, sParams);

        // If Draco is NOT also enabled, apply meshopt stream compression
        // so pure --enable-simplify produces EXT_meshopt_compression output
        if (!enable_draco) {
            MeshoptCompressionParams mParams;
            mParams.enable_compression = true;
            MeshoptCompressionResult mResult;
            if (::compress_mesh_geometry_meshopt(g, mParams, mResult)) {
                meshoptState.compressed = true;
                auto& model = *osgState->model;
                auto& buffer = *osgState->buffer;

                // --- Write each attribute stream into its own bufferView ---
                // attr_streams order: [0]=pos, [1]=normal(opt), [2]=texcoord(opt)
                int sIdx = 0;
                for (size_t ai = 0; ai < mResult.attr_streams.size(); ++ai) {
                    auto& attr = mResult.attr_streams[ai];
                    unsigned bufStart = (unsigned)buffer.data.size();
                    buffer.data.insert(buffer.data.end(), attr.data.begin(), attr.data.end());
                    alignment_buffer(buffer.data);

                    tinygltf::BufferView bv;
                    bv.buffer = 0;
                    bv.byteOffset = (int)bufStart;
                    bv.byteLength = (int)(buffer.data.size() - bufStart);
                    int bvIdx = (int)model.bufferViews.size();
                    model.bufferViews.push_back(bv);

                    MeshoptState::StreamInfo si;
                    si.bufferView = bvIdx;
                    si.count = (int)mResult.vertex_count;
                    si.mode = "ATTRIBUTES";
                    si.filter = attr.filter;
                    si.byteStride = attr.byte_stride;
                    meshoptState.streams.push_back(si);

                    // Map stream index to attribute type
                    if (ai == 0)
                        meshoptState.posStreamIdx = sIdx;
                    else if (ai == 1 && attr.component_count == 3)
                        meshoptState.normStreamIdx = sIdx;
                    else
                        meshoptState.uvStreamIdx = sIdx;
                    ++sIdx;
                }

                // --- Write compressed index stream ---
                if (!mResult.index_data.empty()) {
                    unsigned bufStart = (unsigned)buffer.data.size();
                    buffer.data.insert(buffer.data.end(),
                        mResult.index_data.begin(), mResult.index_data.end());
                    alignment_buffer(buffer.data);

                    tinygltf::BufferView bv;
                    bv.buffer = 0;
                    bv.byteOffset = (int)bufStart;
                    bv.byteLength = (int)(buffer.data.size() - bufStart);
                    int bvIdx = (int)model.bufferViews.size();
                    model.bufferViews.push_back(bv);

                    MeshoptState::StreamInfo si;
                    si.bufferView = bvIdx;
                    si.count = (int)mResult.index_count;
                    si.mode = "TRIANGLES";
                    si.filter = "NONE";
                    si.byteStride = 0;
                    meshoptState.streams.push_back(si);
                    meshoptState.idxStreamIdx = sIdx;
                }
            }
        }
    }

    // ================================================================
    // Step 2: Draco vertex compression
    // ================================================================
    DracoState dracoState;
    if (enable_draco) {
        std::vector<unsigned char> compressed_data;
        size_t compressed_size = 0;
        DracoCompressionParams draco_params;
        draco_params.enable_compression = true;
        draco_params.position_quantization_bits = draco_pos_bits;
        draco_params.normal_quantization_bits = draco_normal_bits;
        draco_params.tex_coord_quantization_bits = draco_uv_bits;
        int dracoPosId = -1, dracoNormId = -1, dracoTexId = -1, dracoBatchId = -1;
        bool ok = ::compress_mesh_geometry(g, draco_params,
            compressed_data, compressed_size,
            &dracoPosId, &dracoNormId, &dracoTexId, &dracoBatchId, nullptr);
        if (ok && compressed_size > 0) {
            auto& model = *osgState->model;
            auto& buffer = *osgState->buffer;
            alignment_buffer(buffer.data);
            unsigned bufOffset = (unsigned)buffer.data.size();
            buffer.data.resize(bufOffset + compressed_size);
            std::memcpy(buffer.data.data() + bufOffset, compressed_data.data(), compressed_size);

            tinygltf::BufferView bv;
            bv.buffer = 0;
            bv.byteOffset = (int)bufOffset;
            bv.byteLength = (int)compressed_size;
            int bvIdx = (int)model.bufferViews.size();
            model.bufferViews.push_back(bv);

            dracoState.compressed = true;
            dracoState.bufferView = bvIdx;
            dracoState.posId = dracoPosId;
            dracoState.normId = dracoNormId;
            dracoState.texId = dracoTexId;
            dracoState.batchId = dracoBatchId;
        }
    }

    // ================================================================
    // Step 3: Write per-primitive glTF data
    // ================================================================
    int pmtV = -1, pmtN = -1, pmtT = -1;
    for (unsigned k = 0; k < g->getNumPrimitiveSets(); k++) {
        osg::PrimitiveSet* ps = g->getPrimitiveSet(k);
        write_primitive(g, ps, osgState, &pmtV, &pmtN, &pmtT, &dracoState, &meshoptState);
    }
}

// ============================================================
// Core: OSG Node → GLB buffer (shared by osgb2glb_buf and convert_one_tile)
// Takes a pre-loaded node and its parent path.
// ============================================================
bool osgb2glb_buf_from_node(osg::Node* root, std::string parent_path,
                                    std::string& glb_buff, MeshInfo& mesh_info,
                                    int node_type, bool enable_texture_compress,
                                    bool enable_meshopt, bool enable_draco, bool enable_unlit,
                                    double simplify_ratio,
                                    int draco_pos_bits, int draco_normal_bits, int draco_uv_bits,
                                    int ktx2_quality) {
    static std::atomic<int64_t> total_info_us{0};
    static std::atomic<int64_t> total_geom_us{0};
    static std::atomic<int64_t> total_tex_us{0};
    static std::atomic<int64_t> total_glb_us{0};
    static std::atomic<size_t> call_n{0};

    auto t0 = std::chrono::steady_clock::now();

    InfoVisitor infoVisitor(parent_path, node_type == -1);
    root->accept(infoVisitor);
    if (node_type == 2 || infoVisitor.geometry_array.empty()) {
        infoVisitor.geometry_array = infoVisitor.other_geometry_array;
        infoVisitor.texture_array = infoVisitor.other_texture_array;
    }
    if (infoVisitor.geometry_array.empty()) return false;

    osgUtil::SmoothingVisitor sv;
    root->accept(sv);

    auto t1 = std::chrono::steady_clock::now();

    tinygltf::TinyGLTF gltf;
    tinygltf::Model model;
    tinygltf::Buffer buffer;

    OsgBuildState osgState = { &buffer, &model,
        osg::Vec3f(-1e38f,-1e38f,-1e38f), osg::Vec3f(1e38f,1e38f,1e38f), -1, -1 };

    model.meshes.resize(1);
    int primIdx = 0;

    for (auto g : infoVisitor.geometry_array) {
        if (!g->getVertexArray() || g->getVertexArray()->getDataSize() == 0)
            continue;

        write_osgGeometry(g, &osgState, enable_meshopt, enable_draco,
                          simplify_ratio, draco_pos_bits, draco_normal_bits, draco_uv_bits);

        if (!infoVisitor.texture_array.empty()) {
            for (unsigned k = 0; k < g->getNumPrimitiveSets(); k++) {
                auto tex = infoVisitor.texture_map[g];
                if (tex) {
                    int matIdx = 0;
                    for (auto t : infoVisitor.texture_array) {
                        if (tex == t) break;
                        matIdx++;
                    }
                    model.meshes[0].primitives[primIdx].material = matIdx;
                }
                primIdx++;
            }
        }
    }

    if (model.meshes[0].primitives.empty()) return false;

    for (auto& prim : model.meshes[0].primitives) {
        if (prim.material < 0) prim.material = 0;
    }

    auto t2 = std::chrono::steady_clock::now();  // geometry done

    mesh_info.min = { (double)osgState.point_min.x(), (double)osgState.point_min.y(), (double)osgState.point_min.z() };
    mesh_info.max = { (double)osgState.point_max.x(), (double)osgState.point_max.y(), (double)osgState.point_max.z() };

    // Process textures via mesh_processor (KTX2 if enabled, JPEG fallback)
    for (auto tex : infoVisitor.texture_array) {
        unsigned bufStart = (unsigned)buffer.data.size();
        std::vector<unsigned char> imgData;
        std::string mimeType;

        if (::process_texture(tex, imgData, mimeType, enable_texture_compress, ktx2_quality)) {
            buffer.data.insert(buffer.data.end(), imgData.begin(), imgData.end());

            tinygltf::Image imgObj;
            imgObj.mimeType = mimeType;
            imgObj.bufferView = (int)model.bufferViews.size();
            model.images.push_back(imgObj);

            tinygltf::BufferView bfv;
            bfv.buffer = 0;
            bfv.byteOffset = (int)bufStart;
            alignment_buffer(buffer.data);
            bfv.byteLength = (int)(buffer.data.size() - bufStart);
            model.bufferViews.push_back(bfv);
        }
    }

    auto t3 = std::chrono::steady_clock::now();  // textures done

    // Node
    tinygltf::Node node; node.mesh = 0;
    model.nodes.push_back(node);

    // Scene
    tinygltf::Scene scene; scene.nodes.push_back(0);
    model.scenes = { scene };
    model.defaultScene = 0;

    // Sampler
    tinygltf::Sampler samp;
    samp.magFilter = TINYGLTF_TEXTURE_FILTER_LINEAR;
    samp.minFilter = TINYGLTF_TEXTURE_FILTER_NEAREST_MIPMAP_LINEAR;
    samp.wrapS = TINYGLTF_TEXTURE_WRAP_REPEAT;
    samp.wrapT = TINYGLTF_TEXTURE_WRAP_REPEAT;
    model.samplers = { samp };

    // Extensions
    if (enable_unlit) {
        model.extensionsRequired.push_back("KHR_materials_unlit");
        model.extensionsUsed.push_back("KHR_materials_unlit");
    }
    if (enable_texture_compress) {
        model.extensionsRequired.push_back("KHR_texture_basisu");
        model.extensionsUsed.push_back("KHR_texture_basisu");
    }
    if (enable_draco) {
        model.extensionsRequired.push_back("KHR_draco_mesh_compression");
        model.extensionsUsed.push_back("KHR_draco_mesh_compression");
    }
    if (enable_meshopt && !enable_draco) {
        model.extensionsRequired.push_back("EXT_meshopt_compression");
        model.extensionsUsed.push_back("EXT_meshopt_compression");
    }

    // Materials
    {
        bool has_images = !model.images.empty();
        if (infoVisitor.texture_array.empty()) {
            auto mat = make_default_material(0.75, 0.75, 0.75);
            if (enable_unlit)
                mat.extensions["KHR_materials_unlit"] = tinygltf::Value(tinygltf::Value::Object());
            model.materials.push_back(mat);
        } else {
            for (size_t i = 0; i < infoVisitor.texture_array.size(); i++) {
                auto mat = make_default_material(0.75, 0.75, 0.75);
                if (enable_unlit)
                    mat.extensions["KHR_materials_unlit"] = tinygltf::Value(tinygltf::Value::Object());
                if (has_images && i < model.images.size())
                    mat.pbrMetallicRoughness.baseColorTexture.index = (int)i;
                model.materials.push_back(mat);
            }
        }
    }

    // Textures
    if (!model.images.empty()) {
        for (size_t i = 0; i < model.images.size(); i++) {
            tinygltf::Texture tex;
            tex.sampler = 0;
            if (enable_texture_compress) {
                tinygltf::Value::Object basisu;
                basisu["source"] = tinygltf::Value((int)i);
                tex.extensions["KHR_texture_basisu"] = tinygltf::Value(basisu);
            } else {
                tex.source = (int)i;
            }
            model.textures.push_back(tex);
        }
    }

    model.asset.version = "2.0";
    model.asset.generator = "osgb2b3dm-cpp";

    model.buffers.push_back(buffer);

    std::ostringstream ss;
    bool ok = gltf.WriteGltfSceneToStream(&model, ss, false, true);

    auto t4 = std::chrono::steady_clock::now();  // GLB done

    // Accumulate per-phase timing for diagnostics
    using namespace std::chrono;
    total_info_us += duration_cast<microseconds>(t1 - t0).count();
    total_geom_us += duration_cast<microseconds>(t2 - t1).count();
    total_tex_us  += duration_cast<microseconds>(t3 - t2).count();
    total_glb_us  += duration_cast<microseconds>(t4 - t3).count();
    size_t n = call_n.fetch_add(1) + 1;
    if (n % 200 == 0 || n == 1) {
        auto to_ms = [](int64_t us) { return us / 1000; };
        LOG_I("  Per-tile phase timing (%zu tiles): info=%lldms geom=%lldms tex=%lldms glb=%lldms",
              n, to_ms(total_info_us.load()), to_ms(total_geom_us.load()),
              to_ms(total_tex_us.load()), to_ms(total_glb_us.load()));
    }

    if (ok) {
        glb_buff = ss.str();
        return true;
    }
    return false;
}

bool osgb2glb_buf(std::string path, std::string& glb_buff, MeshInfo& mesh_info,
                  int node_type, bool enable_texture_compress,
                  bool enable_meshopt, bool enable_draco, bool enable_unlit,
                  double simplify_ratio,
                  int draco_pos_bits, int draco_normal_bits, int draco_uv_bits,
                  int ktx2_quality) {
    vector<string> fileNames = { path };
    std::string parent_path = get_parent(path);

    static bool logged = false;
    if (!logged) { log_osg_plugin_info(); logged = true; }

    osg::ref_ptr<osg::Node> root = osgDB::readNodeFiles(fileNames);
    if (!root.valid()) return false;

    return osgb2glb_buf_from_node(root.get(), parent_path,
                                   glb_buff, mesh_info, node_type,
                                   enable_texture_compress, enable_meshopt,
                                   enable_draco, enable_unlit,
                                   simplify_ratio,
                                   draco_pos_bits, draco_normal_bits, draco_uv_bits,
                                   ktx2_quality);
}

// ============================================================
// OSGB → B3DM buffer
// ============================================================
bool osgb2b3dm_buf(std::string path, std::string& b3dm_buf, TileBox& tile_box,
                   int node_type, bool enable_texture_compress,
                   bool enable_meshopt, bool enable_draco, bool enable_unlit,
                   double simplify_ratio,
                   int draco_pos_bits, int draco_normal_bits, int draco_uv_bits,
                   int ktx2_quality) {
    using nlohmann::json;

    std::string glb_buf;
    MeshInfo minfo;
    if (!osgb2glb_buf(path, glb_buf, minfo, node_type,
                      enable_texture_compress, enable_meshopt, enable_draco, enable_unlit,
                      simplify_ratio, draco_pos_bits, draco_normal_bits, draco_uv_bits,
                      ktx2_quality))
        return false;

    tile_box.max = minfo.max;
    tile_box.min = minfo.min;

    int mesh_count = 1;

    // Feature Table JSON
    std::string ft_json = "{\"BATCH_LENGTH\":" + std::to_string(mesh_count) + "}";
    while ((ft_json.size() + 28) % 8 != 0) ft_json.push_back(' ');

    // Batch Table JSON
    json batch;
    batch["batchId"] = {0};
    batch["name"] = {"mesh_0"};
    std::string bt_json = batch.dump();
    while (bt_json.size() % 8 != 0) bt_json.push_back(' ');

    int ft_len = (int)ft_json.size();
    int bt_len = (int)bt_json.size();

    // B3DM header
    b3dm_buf = "b3dm";
    int version = 1;
    put_val(b3dm_buf, version);

    int totalLenOffset = (int)b3dm_buf.size();
    put_val(b3dm_buf, 0); // placeholder for total_len
    put_val(b3dm_buf, ft_len);
    put_val(b3dm_buf, 0); // feature_bin_len
    put_val(b3dm_buf, bt_len);
    put_val(b3dm_buf, 0); // batch_bin_len

    b3dm_buf.append(ft_json);
    b3dm_buf.append(bt_json);

    // 8-byte align before GLB
    while (b3dm_buf.size() % 8 != 0) b3dm_buf.push_back(0x00);
    b3dm_buf.append(glb_buf);
    while (b3dm_buf.size() % 8 != 0) b3dm_buf.push_back(0x00);

    // Update total length
    int totalLen = (int)b3dm_buf.size();
    *reinterpret_cast<int*>(&b3dm_buf[totalLenOffset]) = totalLen;

    return true;
}

// ============================================================
// Tile processing
// ============================================================
void do_tile_job(osg_tree& tree, std::string out_path, int max_lvl,
                 bool enable_texture_compress, bool enable_meshopt,
                 bool enable_draco, bool enable_unlit,
                 double simplify_ratio,
                 int draco_pos_bits, int draco_normal_bits, int draco_uv_bits,
                 int ktx2_quality) {
    if (tree.file_name.empty()) return;
    int lvl = get_lvl_num(tree.file_name);
    if (lvl > max_lvl) return;

    if (tree.type > 0) {
        std::string b3dm_buf;
        osgb2b3dm_buf(tree.file_name, b3dm_buf, tree.bbox, tree.type,
                      enable_texture_compress, enable_meshopt, enable_draco, enable_unlit,
                      simplify_ratio, draco_pos_bits, draco_normal_bits, draco_uv_bits,
                      ktx2_quality);

        std::string out_file = out_path + "/" +
            replace(get_file_name(tree.file_name), ".osgb", tree.type != 2 ? ".b3dm" : "o.b3dm");

        if (!b3dm_buf.empty()) {
            write_file(out_file.c_str(), b3dm_buf.data(), (unsigned long)b3dm_buf.size());
        }
    }

    for (auto& i : tree.sub_nodes) {
        do_tile_job(i, out_path, max_lvl,
                    enable_texture_compress, enable_meshopt, enable_draco, enable_unlit,
                    simplify_ratio, draco_pos_bits, draco_normal_bits, draco_uv_bits,
                    ktx2_quality);
    }
}

// ============================================================
// JSON generation for tile tree
// ============================================================
std::string get_boundingBox(TileBox bbox) {
    std::string s = "\"boundingVolume\":{\"box\":[";
    auto v = convert_bbox(bbox);
    for (size_t i = 0; i < v.size(); i++) {
        s += std::to_string(v[i]);
        if (i < v.size() - 1) s += ",";
    }
    s += "]}";
    return s;
}

std::string encode_tile_json(osg_tree& tree, double x, double y) {
    if (tree.bbox.max.empty() || tree.bbox.min.empty()) return "";

    std::string file_name = get_file_name(tree.file_name);
    std::string file_path = get_file_name(get_parent(tree.file_name));

    char buf[512];
    sprintf(buf, "{ \"geometricError\":%.2f,", tree.geometricError);
    std::string tile = buf;
    tile += " \"refine\":\"REPLACE\",";

    std::string bbox_str = get_boundingBox(tree.bbox);
    tile += bbox_str;

    if (tree.type > 0) {
        tile += ", \"content\":{ \"uri\":";
        std::string uri = "./" + replace(file_name, ".osgb", tree.type != 2 ? ".b3dm" : "o.b3dm");
        tile += "\"" + uri + "\",";
        tile += bbox_str;
        tile += "}";
    }

    if (!tree.sub_nodes.empty()) {
        tile += ",\"children\":[";
        for (auto& i : tree.sub_nodes) {
            std::string child = encode_tile_json(i, x, y);
            if (!child.empty()) {
                tile += child + ",";
            }
        }
        if (tile.back() == ',') tile.pop_back();
        tile += "]";
    }
    tile += "}";
    return tile;
}

// ============================================================
// 3D Tiles 1.1: Tile processing — outputs raw .glb (no B3DM)
// ============================================================
void do_tile_job_1_1(osg_tree& tree, std::string out_path, int max_lvl,
                     bool enable_texture_compress, bool enable_meshopt,
                     bool enable_draco, bool enable_unlit,
                     double simplify_ratio,
                     int draco_pos_bits, int draco_normal_bits, int draco_uv_bits,
                     int ktx2_quality) {
    if (tree.file_name.empty()) return;
    int lvl = get_lvl_num(tree.file_name);
    if (lvl > max_lvl) return;

    if (tree.type > 0) {
        // Get raw GLB buffer directly (skip B3DM wrapping)
        std::string glb_buf;
        MeshInfo minfo;
        if (osgb2glb_buf(tree.file_name, glb_buf, minfo, tree.type,
                         enable_texture_compress, enable_meshopt, enable_draco, enable_unlit,
                         simplify_ratio, draco_pos_bits, draco_normal_bits, draco_uv_bits,
                         ktx2_quality)) {
            tree.bbox.max = minfo.max;
            tree.bbox.min = minfo.min;

            std::string out_file = out_path + "/" +
                replace(get_file_name(tree.file_name), ".osgb", tree.type != 2 ? ".glb" : "o.glb");

            if (!glb_buf.empty()) {
                write_file(out_file.c_str(), glb_buf.data(), (unsigned long)glb_buf.size());
            }
        }
    }

    for (auto& i : tree.sub_nodes) {
        do_tile_job_1_1(i, out_path, max_lvl,
                        enable_texture_compress, enable_meshopt, enable_draco, enable_unlit,
                        simplify_ratio, draco_pos_bits, draco_normal_bits, draco_uv_bits,
                        ktx2_quality);
    }
}

// ============================================================
// Single tile conversion (for parallel task pool)
// Always loads from disk — cached_node from Phase 1 contains
// pre-loaded child geometry which would break tile-level bbox.
// ============================================================
bool convert_one_tile(const FlatTile& tile, const osgb_converter::ConvertOptions& opts) {
    std::string glb_buf;
    MeshInfo minfo;

    bool ok = osgb2glb_buf(tile.file_name, glb_buf, minfo, tile.type,
                          opts.enable_texture_compress, opts.enable_meshopt,
                          opts.enable_draco, opts.enable_unlit,
                          opts.simplify_ratio,
                          opts.draco_pos_bits, opts.draco_normal_bits, opts.draco_uv_bits,
                          opts.ktx2_quality);

    if (!ok) return false;

    if (tile.tree) {
        tile.tree->bbox.max = minfo.max;
        tile.tree->bbox.min = minfo.min;
    }

    std::string out_file = tile.out_dir + "/" +
        replace(get_file_name(tile.file_name), ".osgb",
                tile.type != 2 ? ".glb" : "o.glb");

    if (!glb_buf.empty())
        write_file(out_file.c_str(), glb_buf.data(), (unsigned long)glb_buf.size());

    return true;
}

// ============================================================
// Compute GLB buffer and output path for a single tile.
// Pure CPU work — no file I/O. Caller handles bbox write-back
// and file writing. Safe for parallel execution.
// ============================================================
bool compute_tile_output(const FlatTile& tile,
                         const osgb_converter::ConvertOptions& opts,
                         std::string& glb_buf, MeshInfo& minfo,
                         std::string& out_file) {
    static std::atomic<size_t> cache_hits{0};
    static std::atomic<size_t> cache_misses{0};
    bool ok = false;
    bool used_cache = false;

    if (tile.tree && tile.tree->cached_node.valid()) {
        ok = osgb2glb_buf_from_node(
            tile.tree->cached_node.get(),
            get_parent(tile.file_name),
            glb_buf, minfo, tile.type,
            opts.enable_texture_compress, opts.enable_meshopt,
            opts.enable_draco, opts.enable_unlit,
            opts.simplify_ratio,
            opts.draco_pos_bits, opts.draco_normal_bits, opts.draco_uv_bits,
            opts.ktx2_quality);
        used_cache = true;
    } else {
        ok = osgb2glb_buf(tile.file_name, glb_buf, minfo, tile.type,
                         opts.enable_texture_compress, opts.enable_meshopt,
                         opts.enable_draco, opts.enable_unlit,
                         opts.simplify_ratio,
                         opts.draco_pos_bits, opts.draco_normal_bits, opts.draco_uv_bits,
                         opts.ktx2_quality);
    }

    size_t h = cache_hits.fetch_add(used_cache ? 1 : 0) + (used_cache ? 1 : 0);
    size_t m = cache_misses.fetch_add(used_cache ? 0 : 1) + (used_cache ? 0 : 1);
    // Log first few misses and every 200 tiles
    if (!used_cache || (h + m) % 200 == 0 || h + m <= 20) {
        LOG_I("  compute_tile: %s (hits=%zu misses=%zu type=%d)",
              used_cache ? "cached" : "DISK", h, m, tile.type);
    }

    if (!ok) return false;

    out_file = tile.out_dir + "/" +
        replace(get_file_name(tile.file_name), ".osgb",
                tile.type != 2 ? ".glb" : "o.glb");

    return true;
}

// ============================================================
// Single tile conversion that uses pre-loaded cached_node from Phase 1.
// Computes GLB buffer, writes bbox, and writes file to disk.
// Used by the serial path and as a convenience wrapper.
// ============================================================
bool convert_one_tile_from_cached(const FlatTile& tile,
                                   const osgb_converter::ConvertOptions& opts) {
    std::string glb_buf;
    MeshInfo minfo;
    std::string out_file;

    if (!compute_tile_output(tile, opts, glb_buf, minfo, out_file))
        return false;

    if (tile.tree) {
        tile.tree->bbox.max = minfo.max;
        tile.tree->bbox.min = minfo.min;
    }

    if (!glb_buf.empty())
        write_file(out_file.c_str(), glb_buf.data(), (unsigned long)glb_buf.size());

    return true;
}

// ============================================================
// Recursively collect all tiles into a flat list for parallel processing
// ============================================================
void collect_flat_tiles(osg_tree& tree, const std::string& out_dir,
                        std::vector<FlatTile>& out) {
    if (tree.file_name.empty()) return;
    if (tree.type > 0) {
        out.push_back({tree.file_name, out_dir, tree.type, &tree});
    }
    for (auto& sub : tree.sub_nodes) {
        collect_flat_tiles(sub, out_dir, out);
    }
}

// ============================================================
// 3D Tiles 1.1: JSON generation with 3DTILES_content_gltf
// ============================================================
std::string encode_tile_json_1_1(osg_tree& tree, double x, double y) {
    if (tree.bbox.max.empty() || tree.bbox.min.empty()) return "";

    std::string file_name = get_file_name(tree.file_name);
    std::string file_path = get_file_name(get_parent(tree.file_name));

    char buf[512];
    sprintf(buf, "{ \"geometricError\":%.2f,", tree.geometricError);
    std::string tile = buf;
    tile += " \"refine\":\"REPLACE\",";

    std::string bbox_str = get_boundingBox(tree.bbox);
    tile += bbox_str;

    if (tree.type > 0) {
        tile += ", \"content\":{ \"uri\":";
        std::string uri = "./" + replace(file_name, ".osgb", tree.type != 2 ? ".glb" : "o.glb");
        tile += "\"" + uri + "\",";
        // 3D Tiles 1.1: declare 3DTILES_content_gltf extension on content
        tile += " \"extensions\":{\"3DTILES_content_gltf\":{}},";
        tile += bbox_str;
        tile += "}";
    }

    if (!tree.sub_nodes.empty()) {
        tile += ",\"children\":[";
        for (auto& i : tree.sub_nodes) {
            std::string child = encode_tile_json_1_1(i, x, y);
            if (!child.empty()) {
                tile += child + ",";
            }
        }
        if (tile.back() == ',') tile.pop_back();
        tile += "]";
    } else {
        tile += ",\"children\":[]";
    }
    tile += "}";
    return tile;
}

// ============================================================
// Root tile reconstruction helpers
// ============================================================

// Returns the coarsest-LOD file for a tile tree: the tree root
// (file without _Lxx suffix, e.g. Tile_-051_+050.osgb).
// Optionally outputs its geometricError (already computed by calc_geometric_error).
std::string find_coarsest_node(const osg_tree& tree, double* out_ge) {
    // The root node (tree.file_name, no L suffix, get_lvl_num → -1) is the
    // coarsest LOD. calc_geometric_error() has already computed its GE.
    // No need to recurse — the tree root IS the coarsest node.
    if (out_ge) *out_ge = tree.geometricError;
    return tree.file_name;
}

// Merge multiple coarsest-LOD OSGB files into a single root GLB.
// Each file is loaded independently, its geometry SVD-corrected by
// InfoVisitor, and all primitives are packed into one tinygltf::Model.
bool build_merged_root_glb(
    const std::vector<std::string>& coarsest_paths,
    std::string& out_glb_buf,
    TileBox& out_bbox,
    bool enable_texture_compress,
    bool enable_meshopt,
    bool enable_draco,
    bool enable_unlit,
    int top_texture_max_size,
    double simplify_ratio,
    int draco_pos_bits, int draco_normal_bits, int draco_uv_bits,
    int ktx2_quality)
{
    if (coarsest_paths.empty()) return false;

    tinygltf::TinyGLTF gltf;
    tinygltf::Model model;
    tinygltf::Buffer buffer;

    std::vector<osg::Geometry*> all_geometry;
    std::vector<osg::Texture*> all_textures;         // canonical textures (deduplicated)
    std::map<size_t, size_t> tex_hash_to_idx;        // hash → index in all_textures
    std::map<osg::Geometry*, osg::Texture*> geom_to_tex;  // updated to canonical ptrs

    osg::Vec3f global_min(1e38f, 1e38f, 1e38f);
    osg::Vec3f global_max(-1e38f, -1e38f, -1e38f);

    // Helper: compute hash from texture image data for deduplication
    auto hash_texture_image = [](osg::Texture* tex) -> size_t {
        if (!tex || tex->getNumImages() == 0) return 0;
        osg::Image* img = tex->getImage(0);
        if (!img || !img->data()) return 0;
        size_t h = std::hash<int>{}(img->s());
        h ^= std::hash<int>{}(img->t()) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<int>{}((int)img->getPixelFormat()) + 0x9e3779b9 + (h << 6) + (h >> 2);
        // Hash raw pixel bytes (sample up to 4KB for speed)
        int total = img->getImageSizeInBytes();
        int step = std::max(1, total / 4096);
        const unsigned char* data = img->data();
        for (int i = 0; i < total; i += step)
            h ^= std::hash<unsigned char>{}(data[i]) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    };

    // Keep loaded nodes alive until the end of this function.
    // Geometry/texture pointers stored in all_geometry/all_textures are
    // raw pointers into these node graphs — destroying the ref_ptr would
    // free the entire OSG node tree, making all pointers dangling.
    std::vector<osg::ref_ptr<osg::Node>> loaded_nodes;

    // Phase 1: Load all coarsest files and collect geometry/textures
    for (const auto& fpath : coarsest_paths) {
        std::string parent_path = get_parent(fpath);
        std::vector<std::string> fileNames = { fpath };

        osg::ref_ptr<osg::Node> root = osgDB::readNodeFiles(fileNames);
        if (!root.valid()) {
            LOG_W("build_merged_root_glb: failed to load %s, skipping", fpath.c_str());
            continue;
        }

        InfoVisitor infoVisitor(parent_path, false);
        root->accept(infoVisitor);

        // Use fallback geometry if PagedLOD collection is empty
        auto& geoms = infoVisitor.geometry_array;
        auto& textures = infoVisitor.texture_array;
        if (geoms.empty() && !infoVisitor.other_geometry_array.empty()) {
            geoms = infoVisitor.other_geometry_array;
            textures = infoVisitor.other_texture_array;
        }

        if (geoms.empty()) {
            LOG_W("build_merged_root_glb: no geometry in %s, skipping", fpath.c_str());
            continue;
        }

        // Collect geometry. Deduplicate textures by image content hash.
        for (auto* g : geoms) {
            all_geometry.push_back(g);
            auto it = infoVisitor.texture_map.find(g);
            if (it != infoVisitor.texture_map.end()) {
                osg::Texture* raw_tex = it->second;
                size_t h = hash_texture_image(raw_tex);
                auto dedup_it = tex_hash_to_idx.find(h);
                if (dedup_it != tex_hash_to_idx.end()) {
                    // Already seen this texture — reuse canonical pointer
                    geom_to_tex[g] = all_textures[dedup_it->second];
                } else {
                    // First occurrence — register as canonical
                    tex_hash_to_idx[h] = all_textures.size();
                    all_textures.push_back(raw_tex);
                    geom_to_tex[g] = raw_tex;
                }
            }
        }

        // Keep root alive — geometry/texture raw pointers point into it
        loaded_nodes.push_back(root);
    }

    if (all_geometry.empty()) {
        LOG_E("build_merged_root_glb: no geometry from any source file");
        return false;
    }

    LOG_I("build_merged_root_glb: merging %zu geometries, %zu textures",
          all_geometry.size(), all_textures.size());

    // Phase 2: Build glTF model
    OsgBuildState osgState = { &buffer, &model,
        osg::Vec3f(-1e38f,-1e38f,-1e38f), osg::Vec3f(1e38f,1e38f,1e38f), -1, -1 };

    model.meshes.resize(1);

    // Build index mapping: texture* -> material index
    std::map<osg::Texture*, int> tex_to_mat;
    {
        int idx = 0;
        for (auto* t : all_textures) tex_to_mat[t] = idx++;
    }

    int primIdx = 0;
    for (auto* g : all_geometry) {
        if (!g->getVertexArray() || g->getVertexArray()->getDataSize() == 0)
            continue;

        write_osgGeometry(g, &osgState, enable_meshopt, enable_draco,
                          simplify_ratio, draco_pos_bits, draco_normal_bits, draco_uv_bits);

        // Assign material to each new primitive
        auto* tex = geom_to_tex[g];
        int matIdx = tex ? tex_to_mat[tex] : 0;
        for (unsigned k = 0; k < g->getNumPrimitiveSets(); k++) {
            if (primIdx < (int)model.meshes[0].primitives.size()) {
                model.meshes[0].primitives[primIdx].material = matIdx;
            }
            primIdx++;
        }

        // Track global bounding box from osgState tracking
        global_min.x() = std::min(global_min.x(), osgState.point_min.x());
        global_min.y() = std::min(global_min.y(), osgState.point_min.y());
        global_min.z() = std::min(global_min.z(), osgState.point_min.z());
        global_max.x() = std::max(global_max.x(), osgState.point_max.x());
        global_max.y() = std::max(global_max.y(), osgState.point_max.y());
        global_max.z() = std::max(global_max.z(), osgState.point_max.z());
    }

    if (model.meshes[0].primitives.empty()) {
        LOG_E("build_merged_root_glb: no primitives generated");
        return false;
    }

    // Fix any -1 material indices
    for (auto& prim : model.meshes[0].primitives) {
        if (prim.material < 0) prim.material = 0;
    }

    // Phase 3: Process textures (with optional downsampling for root overview)
    for (auto* tex : all_textures) {
        // Downsample texture if max_size is set and image exceeds it
        if (top_texture_max_size > 0 && tex && tex->getNumImages() > 0) {
            osg::Image* img = tex->getImage(0);
            if (img && img->data()) {
                int w = img->s(), h = img->t();
                int max_dim = std::max(w, h);
                if (max_dim > top_texture_max_size) {
                    // Compute new dimensions preserving aspect ratio
                    double scale = (double)top_texture_max_size / max_dim;
                    int new_w = std::max(1, (int)(w * scale));
                    int new_h = std::max(1, (int)(h * scale));
                    int channels = img->getPixelFormat() == GL_RGB ? 3 : 4;

                    std::vector<unsigned char> resized(new_w * new_h * channels);

                    LOG_I("  downsampling texture %dx%d → %dx%d (%d ch)",
                          w, h, new_w, new_h, channels);

                    unsigned char* result = stbir_resize_uint8_linear(
                        img->data(), w, h, img->getRowStepInBytes(),
                        resized.data(), new_w, new_h, new_w * channels,
                        (stbir_pixel_layout)channels);

                    if (result) {
                        // Replace image data with downsampled version
                        img->setImage(new_w, new_h, 1,
                            img->getInternalTextureFormat(),
                            img->getPixelFormat(), img->getDataType(),
                            resized.data(), osg::Image::NO_DELETE);
                        // Steal the resized buffer so OSG doesn't copy
                        unsigned char* stolen = (unsigned char*)malloc(resized.size());
                        memcpy(stolen, resized.data(), resized.size());
                        img->setImage(new_w, new_h, 1,
                            img->getInternalTextureFormat(),
                            img->getPixelFormat(), img->getDataType(),
                            stolen, osg::Image::USE_MALLOC_FREE);
                    }
                }
            }
        }

        unsigned bufStart = (unsigned)buffer.data.size();
        std::vector<unsigned char> imgData;
        std::string mimeType;

        if (::process_texture(tex, imgData, mimeType, enable_texture_compress, ktx2_quality)) {
            buffer.data.insert(buffer.data.end(), imgData.begin(), imgData.end());

            tinygltf::Image imgObj;
            imgObj.mimeType = mimeType;
            imgObj.bufferView = (int)model.bufferViews.size();
            model.images.push_back(imgObj);

            tinygltf::BufferView bfv;
            bfv.buffer = 0;
            bfv.byteOffset = (int)bufStart;
            alignment_buffer(buffer.data);
            bfv.byteLength = (int)(buffer.data.size() - bufStart);
            model.bufferViews.push_back(bfv);
        }
    }

    // Phase 4: Build glTF scene objects
    tinygltf::Node node; node.mesh = 0;
    model.nodes.push_back(node);

    tinygltf::Scene scene; scene.nodes.push_back(0);
    model.scenes = { scene };
    model.defaultScene = 0;

    tinygltf::Sampler samp;
    samp.magFilter = TINYGLTF_TEXTURE_FILTER_LINEAR;
    samp.minFilter = TINYGLTF_TEXTURE_FILTER_NEAREST_MIPMAP_LINEAR;
    samp.wrapS = TINYGLTF_TEXTURE_WRAP_REPEAT;
    samp.wrapT = TINYGLTF_TEXTURE_WRAP_REPEAT;
    model.samplers = { samp };

    // Extensions
    if (enable_unlit) {
        model.extensionsRequired.push_back("KHR_materials_unlit");
        model.extensionsUsed.push_back("KHR_materials_unlit");
    }
    if (enable_texture_compress) {
        model.extensionsRequired.push_back("KHR_texture_basisu");
        model.extensionsUsed.push_back("KHR_texture_basisu");
    }
    bool has_draco = enable_draco;
    if (has_draco) {
        model.extensionsRequired.push_back("KHR_draco_mesh_compression");
        model.extensionsUsed.push_back("KHR_draco_mesh_compression");
    }
    if (enable_meshopt && !has_draco) {
        model.extensionsRequired.push_back("EXT_meshopt_compression");
        model.extensionsUsed.push_back("EXT_meshopt_compression");
    }

    // Materials
    bool has_images = !model.images.empty();
    if (all_textures.empty() || !has_images) {
        auto mat = make_default_material(0.75, 0.75, 0.75);
        if (enable_unlit)
            mat.extensions["KHR_materials_unlit"] = tinygltf::Value(tinygltf::Value::Object());
        model.materials.push_back(mat);
    } else {
        for (size_t i = 0; i < all_textures.size(); i++) {
            auto mat = make_default_material(0.75, 0.75, 0.75);
            if (enable_unlit)
                mat.extensions["KHR_materials_unlit"] = tinygltf::Value(tinygltf::Value::Object());
            if (has_images && i < model.images.size())
                mat.pbrMetallicRoughness.baseColorTexture.index = (int)i;
            model.materials.push_back(mat);
        }
    }

    // Textures
    if (!model.images.empty()) {
        for (size_t i = 0; i < model.images.size(); i++) {
            tinygltf::Texture tex;
            tex.sampler = 0;
            if (enable_texture_compress) {
                tinygltf::Value::Object basisu;
                basisu["source"] = tinygltf::Value((int)i);
                tex.extensions["KHR_texture_basisu"] = tinygltf::Value(basisu);
            } else {
                tex.source = (int)i;
            }
            model.textures.push_back(tex);
        }
    }

    model.asset.version = "2.0";
    model.asset.generator = "osgb2b3dm-cpp-root-reconstruct";

    // CRITICAL: buffer must be in model.buffers for BIN chunk to be written
    model.buffers.push_back(buffer);

    // Phase 5: Serialize to GLB
    std::ostringstream ss;
    if (!gltf.WriteGltfSceneToStream(&model, ss, false, true)) {
        LOG_E("build_merged_root_glb: WriteGltfSceneToStream failed");
        return false;
    }

    out_glb_buf = ss.str();

    // Populate output bounding box
    out_bbox.min = { (double)global_min.x(), (double)global_min.y(), (double)global_min.z() };
    out_bbox.max = { (double)global_max.x(), (double)global_max.y(), (double)global_max.z() };

    LOG_I("build_merged_root_glb: success, GLB size=%zu bytes, bbox=[%.2f..%.2f, %.2f..%.2f, %.2f..%.2f]",
          out_glb_buf.size(),
          global_min.x(), global_max.x(), global_min.y(), global_max.y(),
          global_min.z(), global_max.z());

    return true;
}

// ============================================================
// Quadtree HLOD implementation
// ============================================================

bool parse_tile_grid_coords(const std::string& stem, int& out_x, int& out_y) {
    // Expected format: "Tile_-XXX_+YYY" or "Tile_+XXX_-YYY"
    // Find the two underscore-separated number parts after "Tile_"
    if (stem.size() < 7 || stem.substr(0, 5) != "Tile_") return false;

    std::string rest = stem.substr(5); // e.g. "-001_+050"
    auto us_pos = rest.find('_');
    if (us_pos == std::string::npos) return false;

    std::string xs = rest.substr(0, us_pos);  // e.g. "-001"
    std::string ys = rest.substr(us_pos + 1); // e.g. "+050"

    try {
        out_x = std::stoi(xs);
        out_y = std::stoi(ys);
    } catch (...) {
        return false;
    }
    return true;
}

SpatialGrid build_spatial_grid(
    const std::vector<std::string>& tile_stems,
    const std::vector<std::string>& coarsest_paths,
    const std::vector<TileBox>& bboxes,
    const std::vector<double>& coarsest_ges)
{
    SpatialGrid grid;
    size_t n = std::min({tile_stems.size(), coarsest_paths.size(), bboxes.size(), coarsest_ges.size()});

    for (size_t i = 0; i < n; i++) {
        GridCell cell;
        cell.stem = tile_stems[i];
        if (!parse_tile_grid_coords(cell.stem, cell.grid_x, cell.grid_y)) {
            LOG_W("build_spatial_grid: failed to parse coords from '%s', skipping",
                  cell.stem.c_str());
            continue;
        }
        cell.coarsest_path = coarsest_paths[i];
        cell.bbox = bboxes[i];
        cell.geometricError = coarsest_ges[i];  // Inherit from coarsest PagedLOD tile

        // GLB URI for the coarsest tile: "./Tile_-001_+050.glb"
        // This is the relative path within the tile's output directory
        std::string glb_name = replace(get_file_name(cell.coarsest_path), ".osgb", ".glb");
        cell.glb_uri = "./" + glb_name;

        grid[cell.grid_x][cell.grid_y] = std::move(cell);
    }

    LOG_I("build_spatial_grid: %zu tiles placed in grid", n);
    return grid;
}

double calc_level_ratio(int level, double base_ratio) {
    // Each level covers 4x the area → keep 1/4 the detail
    double r = base_ratio * std::pow(0.25, level);
    return std::max(0.01, r);
}

// ============================================================
// General merge: multiple OSGB files → one GLB with level-based simplification
// ============================================================
bool build_merged_glb(
    const std::vector<std::string>& osgb_paths,
    int quadtree_level,
    std::string& out_glb_buf,
    TileBox& out_bbox,
    bool enable_texture_compress,
    bool enable_meshopt,
    bool enable_draco,
    bool enable_unlit,
    int top_texture_max_size,
    double simplify_ratio,
    int draco_pos_bits, int draco_normal_bits, int draco_uv_bits,
    int ktx2_quality)
{
    if (osgb_paths.empty()) return false;

    // Compute level-specific simplify ratio
    double level_ratio = calc_level_ratio(quadtree_level, simplify_ratio);
    LOG_I("build_merged_glb: level=%d, ratio=%.4f (%zu tiles)",
          quadtree_level, level_ratio, osgb_paths.size());

    tinygltf::TinyGLTF gltf;
    tinygltf::Model model;
    tinygltf::Buffer buffer;

    std::vector<osg::Geometry*> all_geometry;
    std::vector<osg::Texture*> all_textures;
    std::map<size_t, size_t> tex_hash_to_idx;
    std::map<osg::Geometry*, osg::Texture*> geom_to_tex;

    osg::Vec3f global_min(1e38f, 1e38f, 1e38f);
    osg::Vec3f global_max(-1e38f, -1e38f, -1e38f);

    // Texture dedup helper
    auto hash_texture_image = [](osg::Texture* tex) -> size_t {
        if (!tex || tex->getNumImages() == 0) return 0;
        osg::Image* img = tex->getImage(0);
        if (!img || !img->data()) return 0;
        size_t h = std::hash<int>{}(img->s());
        h ^= std::hash<int>{}(img->t()) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<int>{}((int)img->getPixelFormat()) + 0x9e3779b9 + (h << 6) + (h >> 2);
        int total = img->getImageSizeInBytes();
        int step = std::max(1, total / 4096);
        const unsigned char* data = img->data();
        for (int j = 0; j < total; j += step)
            h ^= std::hash<unsigned char>{}(data[j]) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    };

    std::vector<osg::ref_ptr<osg::Node>> loaded_nodes;

    // Phase 1: Load all OSGB files and collect geometry/textures
    for (const auto& fpath : osgb_paths) {
        std::string parent_path = get_parent(fpath);
        std::vector<std::string> fileNames = { fpath };

        osg::ref_ptr<osg::Node> root = osgDB::readNodeFiles(fileNames);
        if (!root.valid()) {
            LOG_W("build_merged_glb: failed to load %s, skipping", fpath.c_str());
            continue;
        }

        InfoVisitor infoVisitor(parent_path, false);
        root->accept(infoVisitor);

        auto& geoms = infoVisitor.geometry_array;
        auto& textures = infoVisitor.texture_array;
        if (geoms.empty() && !infoVisitor.other_geometry_array.empty()) {
            geoms = infoVisitor.other_geometry_array;
            textures = infoVisitor.other_texture_array;
        }

        if (geoms.empty()) {
            LOG_W("build_merged_glb: no geometry in %s, skipping", fpath.c_str());
            continue;
        }

        for (auto* g : geoms) {
            all_geometry.push_back(g);
            auto it = infoVisitor.texture_map.find(g);
            if (it != infoVisitor.texture_map.end()) {
                osg::Texture* raw_tex = it->second;
                size_t h = hash_texture_image(raw_tex);
                auto dedup_it = tex_hash_to_idx.find(h);
                if (dedup_it != tex_hash_to_idx.end()) {
                    geom_to_tex[g] = all_textures[dedup_it->second];
                } else {
                    tex_hash_to_idx[h] = all_textures.size();
                    all_textures.push_back(raw_tex);
                    geom_to_tex[g] = raw_tex;
                }
            }
        }

        loaded_nodes.push_back(root);
    }

    if (all_geometry.empty()) {
        LOG_E("build_merged_glb: no geometry from any source file");
        return false;
    }

    LOG_I("build_merged_glb: merging %zu geometries, %zu textures (level=%d, ratio=%.4f)",
          all_geometry.size(), all_textures.size(), quadtree_level, level_ratio);

    // Phase 2: Build glTF model
    OsgBuildState osgState = { &buffer, &model,
        osg::Vec3f(-1e38f,-1e38f,-1e38f), osg::Vec3f(1e38f,1e38f,1e38f), -1, -1 };

    model.meshes.resize(1);

    std::map<osg::Texture*, int> tex_to_mat;
    {
        int idx = 0;
        for (auto* t : all_textures) tex_to_mat[t] = idx++;
    }

    int primIdx = 0;
    for (auto* g : all_geometry) {
        if (!g->getVertexArray() || g->getVertexArray()->getDataSize() == 0)
            continue;

        write_osgGeometry(g, &osgState, enable_meshopt, enable_draco,
                          level_ratio, draco_pos_bits, draco_normal_bits, draco_uv_bits);

        auto* tex = geom_to_tex[g];
        int matIdx = tex ? tex_to_mat[tex] : 0;
        for (unsigned k = 0; k < g->getNumPrimitiveSets(); k++) {
            if (primIdx < (int)model.meshes[0].primitives.size()) {
                model.meshes[0].primitives[primIdx].material = matIdx;
            }
            primIdx++;
        }

        global_min.x() = std::min(global_min.x(), osgState.point_min.x());
        global_min.y() = std::min(global_min.y(), osgState.point_min.y());
        global_min.z() = std::min(global_min.z(), osgState.point_min.z());
        global_max.x() = std::max(global_max.x(), osgState.point_max.x());
        global_max.y() = std::max(global_max.y(), osgState.point_max.y());
        global_max.z() = std::max(global_max.z(), osgState.point_max.z());
    }

    if (model.meshes[0].primitives.empty()) {
        LOG_E("build_merged_glb: no primitives generated");
        return false;
    }

    for (auto& prim : model.meshes[0].primitives) {
        if (prim.material < 0) prim.material = 0;
    }

    // Phase 3: Process textures (with optional downsampling)
    for (auto* tex : all_textures) {
        if (top_texture_max_size > 0 && tex && tex->getNumImages() > 0) {
            osg::Image* img = tex->getImage(0);
            if (img && img->data()) {
                int w = img->s(), h = img->t();
                int max_dim = std::max(w, h);
                if (max_dim > top_texture_max_size) {
                    double scale = (double)top_texture_max_size / max_dim;
                    int new_w = std::max(1, (int)(w * scale));
                    int new_h = std::max(1, (int)(h * scale));
                    int channels = img->getPixelFormat() == GL_RGB ? 3 : 4;
                    std::vector<unsigned char> resized(new_w * new_h * channels);

                    unsigned char* result = stbir_resize_uint8_linear(
                        img->data(), w, h, img->getRowStepInBytes(),
                        resized.data(), new_w, new_h, new_w * channels,
                        (stbir_pixel_layout)channels);

                    if (result) {
                        unsigned char* stolen = (unsigned char*)malloc(resized.size());
                        memcpy(stolen, resized.data(), resized.size());
                        img->setImage(new_w, new_h, 1,
                            img->getInternalTextureFormat(),
                            img->getPixelFormat(), img->getDataType(),
                            stolen, osg::Image::USE_MALLOC_FREE);
                    }
                }
            }
        }

        unsigned bufStart = (unsigned)buffer.data.size();
        std::vector<unsigned char> imgData;
        std::string mimeType;

        if (::process_texture(tex, imgData, mimeType, enable_texture_compress, ktx2_quality)) {
            buffer.data.insert(buffer.data.end(), imgData.begin(), imgData.end());

            tinygltf::Image imgObj;
            imgObj.mimeType = mimeType;
            imgObj.bufferView = (int)model.bufferViews.size();
            model.images.push_back(imgObj);

            tinygltf::BufferView bfv;
            bfv.buffer = 0;
            bfv.byteOffset = (int)bufStart;
            alignment_buffer(buffer.data);
            bfv.byteLength = (int)(buffer.data.size() - bufStart);
            model.bufferViews.push_back(bfv);
        }
    }

    // Phase 4: Build glTF scene objects
    tinygltf::Node node; node.mesh = 0;
    model.nodes.push_back(node);

    tinygltf::Scene scene; scene.nodes.push_back(0);
    model.scenes = { scene };
    model.defaultScene = 0;

    tinygltf::Sampler samp;
    samp.magFilter = TINYGLTF_TEXTURE_FILTER_LINEAR;
    samp.minFilter = TINYGLTF_TEXTURE_FILTER_NEAREST_MIPMAP_LINEAR;
    samp.wrapS = TINYGLTF_TEXTURE_WRAP_REPEAT;
    samp.wrapT = TINYGLTF_TEXTURE_WRAP_REPEAT;
    model.samplers = { samp };

    if (enable_unlit) {
        model.extensionsRequired.push_back("KHR_materials_unlit");
        model.extensionsUsed.push_back("KHR_materials_unlit");
    }
    if (enable_texture_compress) {
        model.extensionsRequired.push_back("KHR_texture_basisu");
        model.extensionsUsed.push_back("KHR_texture_basisu");
    }
    bool has_draco = enable_draco;
    if (has_draco) {
        model.extensionsRequired.push_back("KHR_draco_mesh_compression");
        model.extensionsUsed.push_back("KHR_draco_mesh_compression");
    }
    if (enable_meshopt && !has_draco) {
        model.extensionsRequired.push_back("EXT_meshopt_compression");
        model.extensionsUsed.push_back("EXT_meshopt_compression");
    }

    // Materials
    bool has_images = !model.images.empty();
    if (all_textures.empty() || !has_images) {
        auto mat = make_default_material(0.75, 0.75, 0.75);
        if (enable_unlit)
            mat.extensions["KHR_materials_unlit"] = tinygltf::Value(tinygltf::Value::Object());
        model.materials.push_back(mat);
    } else {
        for (size_t i = 0; i < all_textures.size(); i++) {
            auto mat = make_default_material(0.75, 0.75, 0.75);
            if (enable_unlit)
                mat.extensions["KHR_materials_unlit"] = tinygltf::Value(tinygltf::Value::Object());
            if (has_images && i < model.images.size())
                mat.pbrMetallicRoughness.baseColorTexture.index = (int)i;
            model.materials.push_back(mat);
        }
    }

    // Textures
    if (!model.images.empty()) {
        for (size_t i = 0; i < model.images.size(); i++) {
            tinygltf::Texture tex;
            tex.sampler = 0;
            if (enable_texture_compress) {
                tinygltf::Value::Object basisu;
                basisu["source"] = tinygltf::Value((int)i);
                tex.extensions["KHR_texture_basisu"] = tinygltf::Value(basisu);
            } else {
                tex.source = (int)i;
            }
            model.textures.push_back(tex);
        }
    }

    model.asset.version = "2.0";
    model.asset.generator = "osgb2b3dm-cpp-hlod-merge";

    model.buffers.push_back(buffer);

    // Phase 5: Serialize to GLB
    std::ostringstream ss;
    if (!gltf.WriteGltfSceneToStream(&model, ss, false, true)) {
        LOG_E("build_merged_glb: WriteGltfSceneToStream failed");
        return false;
    }

    out_glb_buf = ss.str();

    out_bbox.min = { (double)global_min.x(), (double)global_min.y(), (double)global_min.z() };
    out_bbox.max = { (double)global_max.x(), (double)global_max.y(), (double)global_max.z() };

    LOG_I("build_merged_glb: success, GLB size=%zu bytes, level=%d",
          out_glb_buf.size(), quadtree_level);

    return true;
}

// ============================================================
// Collect all leaf OSGB paths under a quadtree node
// ============================================================
void collect_leaf_paths(const QuadNode& node, const SpatialGrid& grid,
                               std::vector<std::string>& paths) {
    if (node.level == 0) {
        // Level 0 node: leaf_coarsest_paths are the coarsest OSGB paths
        // of the grid cells directly under this node.
        paths.insert(paths.end(),
                     node.leaf_coarsest_paths.begin(),
                     node.leaf_coarsest_paths.end());
    } else {
        for (const auto& child : node.children) {
            collect_leaf_paths(child, grid, paths);
        }
    }
}

// ============================================================
// Recursive quadtree builder
// ============================================================
static QuadNode build_quadtree_impl(const SpatialGrid& grid,
                                     int x, int y, int size, int level) {
    // size=1 cells are NOT wrapped — instead, size=2 is the new base case
    // that directly merges 4 grid cells into a level-0 quadtree node.
    if (size == 1) {
        return QuadNode{};  // Empty — grid cell content handled by size=2 parent
    }

    if (size == 2) {
        // New base case: directly collect 4 grid cells (size=1 each) and build
        // a level-0 quadtree node with PagedLOD roots as children.
        QuadNode node;
        node.grid_x = x;
        node.grid_y = y;
        node.grid_size = size;
        node.level = 0;
        node.has_content = false;

        std::pair<int,int> cells[4] = {
            {x,     y    },
            {x+1,   y    },
            {x,     y+1  },
            {x+1,   y+1  }
        };

        double max_cell_ge = 0.0;
        for (auto& [cx, cy] : cells) {
            auto it_x = grid.find(cx);
            if (it_x == grid.end()) continue;
            auto it_y = it_x->second.find(cy);
            if (it_y == it_x->second.end()) continue;

            const auto& cell = it_y->second;
            TileBox cell_bbox = cell.bbox;  // copy for non-const expend_box
            expend_box(node.bbox, cell_bbox);
            max_cell_ge = std::max(max_cell_ge, cell.geometricError);
            node.leaf_stems.push_back(cell.stem);
            node.leaf_coarsest_paths.push_back(cell.coarsest_path);
        }

        if (node.leaf_stems.empty()) {
            return QuadNode{};  // No cells in this 2×2 area
        }

        // Single cell in a 2×2 area: don't merge, just store as-is
        // (multiple cells will be merged, GE doubles)
        if (node.leaf_stems.size() == 1) {
            node.geometricError = max_cell_ge;
        } else {
            node.geometricError = max_cell_ge * 2.0;
        }

        node.has_content = true;  // Will be generated by merge phase
        return node;
    }

    // size >= 4: recursive case, same as before
    int half = size / 2;

    QuadNode node;
    node.grid_x = x;
    node.grid_y = y;
    node.grid_size = size;
    node.level = level;
    node.has_content = false;

    std::pair<int,int> origins[4] = {
        {x,       y},
        {x+half,  y},
        {x,       y+half},
        {x+half,  y+half}
    };

    int child_level = level - 1;
    for (auto& [cx, cy] : origins) {
        QuadNode child = build_quadtree_impl(grid, cx, cy, half, child_level);
        if (child.has_content || !child.children.empty()) {
            expend_box(node.bbox, child.bbox);
            node.children.push_back(std::move(child));
        }
    }

    if (node.children.empty()) {
        return QuadNode{};  // No content in this region
    }

    // Compute geometric error bottom-up.
    // Internal nodes double the max child GE at each level up.
    for (auto& child : node.children) {
        node.geometricError = std::max(node.geometricError, child.geometricError);
    }
    node.geometricError *= 2.0;

    node.has_content = true;  // Will be generated by merge phase

    return node;
}

QuadNode build_quadtree(const SpatialGrid& grid) {
    if (grid.empty()) {
        LOG_E("build_quadtree: empty grid");
        return QuadNode{};
    }

    // Find grid bounds
    int min_x = INT_MAX, max_x = INT_MIN;
    int min_y = INT_MAX, max_y = INT_MIN;
    int cell_count = 0;

    for (const auto& [x, row] : grid) {
        min_x = std::min(min_x, x);
        max_x = std::max(max_x, x);
        for (const auto& [y, cell] : row) {
            min_y = std::min(min_y, y);
            max_y = std::max(max_y, y);
            cell_count++;
        }
    }

    int w = max_x - min_x + 1;
    int h = max_y - min_y + 1;
    int max_dim = std::max(w, h);

    // Compute padded power-of-2 size
    int padded_size = 1;
    while (padded_size < max_dim) padded_size <<= 1;

    // Align origin to the padded grid
    int origin_x = min_x;
    int origin_y = min_y;

    int max_level = (int)std::log2(padded_size) - 1;  // size=2 → level=0

    // Single cell or all cells in one 2×2 block — no HLOD tree needed
    if (max_level < 0) {
        LOG_I("build_quadtree: padded_size=%d, no HLOD levels to build", padded_size);
        return QuadNode{};
    }

    LOG_I("build_quadtree: grid [%d..%d]x[%d..%d] (%dx%d), padded=%d, max_level=%d, cells=%d",
          min_x, max_x, min_y, max_y, w, h, padded_size, max_level, cell_count);

    QuadNode root = build_quadtree_impl(grid, origin_x, origin_y, padded_size, max_level);

    if (root.children.empty() && !root.has_content) {
        LOG_E("build_quadtree: failed to build tree");
        return root;
    }

    // Count nodes at each level
    std::function<void(const QuadNode&, int)> count_nodes = [&](const QuadNode& n, int depth) {
        if (n.children.empty()) return;
        for (const auto& c : n.children) count_nodes(c, depth + 1);
    };

    LOG_I("build_quadtree: root at (%d,%d) size=%d level=%d, children=%zu",
          root.grid_x, root.grid_y, root.grid_size, root.level, root.children.size());

    return root;
}

// ============================================================
// Generate tileset JSON for quadtree hierarchy
// ============================================================
nlohmann::json encode_quadtree_json(
    const QuadNode& node,
    const std::map<std::string, nlohmann::json>& tile_jsons)
{
    // Level 0: first merge level — generate JSON with HLOD content and
    // PagedLOD root tiles as children (looked up from tile_jsons_map).
    if (node.level == 0) {
        nlohmann::json tile;
        tile["geometricError"] = node.geometricError;
        tile["refine"] = "REPLACE";
        tile["boundingVolume"]["box"] = convert_bbox(node.bbox);

        if (node.has_content && !node.glb_uri.empty()) {
            nlohmann::json content;
            content["uri"] = node.glb_uri;
            content["extensions"]["3DTILES_content_gltf"] = nlohmann::json::object();
            TileBox cb = node.bbox;
            cb.extend(0.2);
            content["boundingVolume"]["box"] = convert_bbox(cb);
            tile["content"] = content;
        }

        tile["children"] = nlohmann::json::array();
        for (const auto& stem : node.leaf_stems) {
            auto it = tile_jsons.find(stem);
            if (it != tile_jsons.end()) {
                tile["children"].push_back(it->second);
            }
        }
        return tile;
    }

    // Internal node: generate JSON with HLOD content and quadtree children
    nlohmann::json tile;
    tile["geometricError"] = node.geometricError;
    tile["refine"] = "REPLACE";
    tile["boundingVolume"]["box"] = convert_bbox(node.bbox);

    if (node.has_content && !node.glb_uri.empty()) {
        nlohmann::json content;
        content["uri"] = node.glb_uri;
        content["extensions"]["3DTILES_content_gltf"] = nlohmann::json::object();
        TileBox cb = node.bbox;
        cb.extend(0.2);
        content["boundingVolume"]["box"] = convert_bbox(cb);
        tile["content"] = content;
    }

    if (!node.children.empty()) {
        tile["children"] = nlohmann::json::array();
        for (const auto& child : node.children) {
            nlohmann::json child_json = encode_quadtree_json(child, tile_jsons);
            if (!child_json.empty()) {
                tile["children"].push_back(child_json);
            }
        }
    } else {
        tile["children"] = nlohmann::json::array();
    }

    return tile;
}
