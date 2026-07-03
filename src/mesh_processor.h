#pragma once

#include <vector>
#include <string>
#include <osg/Geometry>

// Forward declarations for Draco
namespace draco { class Mesh; }

// Structure to encapsulate vertex data for mesh processing
struct VertexData {
    float x, y, z;          // Position
    float nx, ny, nz;       // Normal
    float u, v;             // Texture coordinates
    VertexData() : x(0), y(0), z(0), nx(0), ny(0), nz(0), u(0), v(0) {}
};

// Mesh simplification parameters
struct SimplificationParams {
    float target_error = 0.01f;
    float target_ratio = 0.5f;
    bool enable_simplification = false;
    bool preserve_texture_coords = true;
    bool preserve_normals = true;
};

// Draco compression parameters
struct DracoCompressionParams {
    int position_quantization_bits = 11;
    int normal_quantization_bits = 10;
    int tex_coord_quantization_bits = 12;
    int generic_quantization_bits = 8;
    bool enable_compression = false;
};

// Function to compress image data to KTX2 using Basis Universal
bool compress_to_ktx2(const std::vector<unsigned char>& rgba_data, int width, int height,
                      std::vector<unsigned char>& ktx2_data);

// Optimize and simplify mesh using meshoptimizer
bool optimize_and_simplify_mesh(
    std::vector<VertexData>& vertices, size_t& vertex_count,
    std::vector<unsigned int>& indices, size_t original_index_count,
    std::vector<unsigned int>& simplified_indices, size_t& simplified_index_count,
    const SimplificationParams& params);

// Simplify mesh geometry using meshoptimizer
bool simplify_mesh_geometry(osg::Geometry* geometry, const SimplificationParams& params);

// Compress mesh geometry using Draco
bool compress_mesh_geometry(osg::Geometry* geometry, const DracoCompressionParams& params,
                           std::vector<unsigned char>& compressed_data, size_t& compressed_size,
                           int* out_position_att_id = nullptr,
                           int* out_normal_att_id = nullptr,
                           int* out_texcoord_att_id = nullptr,
                           int* out_batchid_att_id = nullptr,
                           const std::vector<float>* batchIds = nullptr);

// Process textures (KTX2 compression)
bool process_texture(osg::Texture* tex, std::vector<unsigned char>& image_data,
                     std::string& mime_type, bool enable_texture_compress = false);
