#include "mesh_processor.h"
#include <cstddef>
#include <osg/Texture>
#include <osg/Image>
#include <osg/Array>
#include <vector>
#include <cstdlib>
#include <cstring>
#include <mutex>

// ============================================================
// Conditional includes for optional dependencies
// ============================================================
#ifdef HAS_BASISU
#include <basisu/encoder/basisu_comp.h>
#include <basisu/transcoder/basisu_transcoder.h>
#endif

#ifdef HAS_MESHOPTIMIZER
#include <meshoptimizer.h>
#endif

#ifdef HAS_DRACO
#include "draco/compression/encode.h"
#include "draco/core/encoder_buffer.h"
#include "draco/mesh/mesh.h"
#endif

// stb_image implementation (needed by tinygltf)
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#ifdef HAS_STB
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#endif

// Helper function to write buffer data
static void write_buf_callback(void* context, void* data, int len) {
    std::vector<char>* buf = (std::vector<char>*)context;
    buf->insert(buf->end(), (char*)data, (char*)data + len);
}

// ============================================================
// KTX2 Compression (requires basisu)
// ============================================================
bool compress_to_ktx2(const std::vector<unsigned char>& rgba_data, int width, int height,
                      std::vector<unsigned char>& ktx2_data) {
#ifdef HAS_BASISU
    try {
        if (rgba_data.empty() || width <= 0 || height <= 0) return false;

        static std::once_flag basisu_init;
        std::call_once(basisu_init, []() { basisu::basisu_encoder_init(); });

        basisu::vector<basisu::image> source_images;
        source_images.push_back(basisu::image(rgba_data.data(), width, height, 4));
        int quality_level = 128;
        std::size_t file_size = 0;

        unsigned int flags = quality_level | basisu::cFlagKTX2 | basisu::cFlagGenMipsWrap | basisu::cFlagThreaded;
        void* pKTX2_data = basisu::basis_compress(
            basist::basis_tex_format::cETC1S, source_images, flags, 1.0f, &file_size, nullptr);

        ktx2_data.assign((unsigned char*)pKTX2_data, (unsigned char*)pKTX2_data + file_size);
        basisu::basis_free_data(pKTX2_data);
        return true;
    } catch (...) { return false; }
#else
    (void)rgba_data; (void)width; (void)height; (void)ktx2_data;
    return false;
#endif
}

// ============================================================
// Mesh Simplification (requires meshoptimizer)
// ============================================================
bool optimize_and_simplify_mesh(
    std::vector<VertexData>& vertices, size_t& vertex_count,
    std::vector<unsigned int>& indices, size_t original_index_count,
    std::vector<unsigned int>& simplified_indices, size_t& simplified_index_count,
    const SimplificationParams& params) {
#ifdef HAS_MESHOPTIMIZER
    size_t target_index_count = static_cast<size_t>(original_index_count * params.target_ratio);

    bool hasNormals = false;
    if (params.preserve_normals && vertex_count > 0) {
        for (size_t i = 0; i < vertex_count; ++i) {
            if (vertices[i].nx != 0.0f || vertices[i].ny != 0.0f || vertices[i].nz != 0.0f) {
                hasNormals = true; break;
            }
        }
    }

    std::vector<unsigned int> remap(vertex_count);
    size_t unique_vertex_count = meshopt_generateVertexRemap(
        remap.data(), indices.data(), original_index_count, vertices.data(), vertex_count, sizeof(VertexData));
    meshopt_remapIndexBuffer(indices.data(), indices.data(), original_index_count, remap.data());

    std::vector<VertexData> remapped_vertices(unique_vertex_count);
    meshopt_remapVertexBuffer(remapped_vertices.data(), vertices.data(), vertex_count, sizeof(VertexData), remap.data());
    vertices = std::move(remapped_vertices);
    vertex_count = unique_vertex_count;

    meshopt_optimizeVertexCache(indices.data(), indices.data(), original_index_count, vertex_count);
    meshopt_optimizeOverdraw(indices.data(), indices.data(), original_index_count, &vertices[0].x, vertex_count, sizeof(VertexData), 1.05f);
    meshopt_optimizeVertexFetch(vertices.data(), indices.data(), original_index_count, vertices.data(), vertex_count, sizeof(VertexData));

    simplified_indices.resize(original_index_count);
    float result_error = 0;

    if (hasNormals) {
        float attr_weights[3] = {0.5f, 0.5f, 0.5f};
        simplified_index_count = meshopt_simplifyWithAttributes(
            simplified_indices.data(), indices.data(), original_index_count,
            &vertices[0].x, vertex_count, sizeof(VertexData),
            &vertices[0].nx, sizeof(VertexData), attr_weights, 3,
            nullptr, target_index_count, params.target_error, 0, &result_error);
    } else {
        simplified_index_count = meshopt_simplify(
            simplified_indices.data(), indices.data(), original_index_count,
            &vertices[0].x, vertex_count, sizeof(VertexData),
            target_index_count, params.target_error, 0, &result_error);
    }
    simplified_indices.resize(simplified_index_count);
    return true;
#else
    (void)vertices; (void)vertex_count; (void)indices; (void)original_index_count;
    (void)simplified_indices; (void)simplified_index_count; (void)params;
    return false;
#endif
}

bool simplify_mesh_geometry(osg::Geometry* geometry, const SimplificationParams& params) {
#ifdef HAS_MESHOPTIMIZER
    if (!params.enable_simplification || !geometry) return false;

    osg::Vec3Array* vertexArray = dynamic_cast<osg::Vec3Array*>(geometry->getVertexArray());
    if (!vertexArray || vertexArray->empty() || geometry->getNumPrimitiveSets() == 0) return false;

    osg::PrimitiveSet* primitiveSet = geometry->getPrimitiveSet(0);
    if (!primitiveSet) return false;

    size_t vertex_count = vertexArray->size();
    osg::Vec3Array* normalArray = dynamic_cast<osg::Vec3Array*>(geometry->getNormalArray());
    bool hasNormals = params.preserve_normals && normalArray && normalArray->size() == vertex_count;
    osg::Vec2Array* texCoordArray = dynamic_cast<osg::Vec2Array*>(geometry->getTexCoordArray(0));
    bool hasTexCoords = params.preserve_texture_coords && texCoordArray && texCoordArray->size() == vertex_count;

    std::vector<VertexData> vertices(vertex_count);
    for (size_t i = 0; i < vertex_count; ++i) {
        const osg::Vec3& v = vertexArray->at(i);
        vertices[i].x = v.x(); vertices[i].y = v.y(); vertices[i].z = v.z();
        if (hasNormals) {
            const osg::Vec3& n = normalArray->at(i);
            vertices[i].nx = n.x(); vertices[i].ny = n.y(); vertices[i].nz = n.z();
        }
        if (hasTexCoords) {
            const osg::Vec2& t = texCoordArray->at(i);
            vertices[i].u = t.x(); vertices[i].v = t.y();
        }
    }

    // Extract indices
    std::vector<unsigned int> indices;
    size_t original_index_count = 0;
    switch (primitiveSet->getType()) {
        case osg::PrimitiveSet::DrawElementsUBytePrimitiveType: {
            auto* de = static_cast<const osg::DrawElementsUByte*>(primitiveSet);
            original_index_count = de->size(); indices.resize(original_index_count);
            for (size_t i = 0; i < original_index_count; ++i) indices[i] = de->at(i);
            break;
        }
        case osg::PrimitiveSet::DrawElementsUShortPrimitiveType: {
            auto* de = static_cast<const osg::DrawElementsUShort*>(primitiveSet);
            original_index_count = de->size(); indices.resize(original_index_count);
            for (size_t i = 0; i < original_index_count; ++i) indices[i] = de->at(i);
            break;
        }
        case osg::PrimitiveSet::DrawElementsUIntPrimitiveType: {
            auto* de = static_cast<const osg::DrawElementsUInt*>(primitiveSet);
            original_index_count = de->size(); indices.resize(original_index_count);
            for (size_t i = 0; i < original_index_count; ++i) indices[i] = de->at(i);
            break;
        }
        case osg::PrimitiveSet::DrawArraysPrimitiveType: {
            auto* da = static_cast<osg::DrawArrays*>(primitiveSet);
            unsigned first = da->getFirst(), count = da->getCount();
            original_index_count = count; indices.resize(count);
            for (unsigned i = 0; i < count; ++i) indices[i] = first + i;
            break;
        }
        default: return false;
    }

    std::vector<unsigned int> simplified_indices;
    size_t simplified_index_count = 0;
    if (!optimize_and_simplify_mesh(vertices, vertex_count, indices, original_index_count,
                                     simplified_indices, simplified_index_count, params))
        return false;

    // Update geometry
    osg::ref_ptr<osg::Vec3Array> newVA = new osg::Vec3Array();
    for (size_t i = 0; i < vertex_count; ++i)
        newVA->push_back(osg::Vec3(vertices[i].x, vertices[i].y, vertices[i].z));
    geometry->setVertexArray(newVA);

    if (hasNormals) {
        osg::ref_ptr<osg::Vec3Array> newNA = new osg::Vec3Array();
        for (size_t i = 0; i < vertex_count; ++i)
            newNA->push_back(osg::Vec3(vertices[i].nx, vertices[i].ny, vertices[i].nz));
        geometry->setNormalArray(newNA);
        geometry->setNormalBinding(osg::Geometry::BIND_PER_VERTEX);
    }
    if (hasTexCoords) {
        osg::ref_ptr<osg::Vec2Array> newTA = new osg::Vec2Array();
        for (size_t i = 0; i < vertex_count; ++i)
            newTA->push_back(osg::Vec2(vertices[i].u, vertices[i].v));
        geometry->setTexCoordArray(0, newTA);
    }

    // Create new primitive set
    osg::DrawElementsUInt* newDE = new osg::DrawElementsUInt(primitiveSet->getMode());
    for (size_t i = 0; i < simplified_index_count; ++i)
        newDE->push_back(simplified_indices[i]);
    geometry->setPrimitiveSet(0, newDE);

    return true;
#else
    (void)geometry; (void)params;
    return false;
#endif
}

// ============================================================
// Draco Compression (requires draco)
// ============================================================
bool compress_mesh_geometry(osg::Geometry* geometry, const DracoCompressionParams& params,
                           std::vector<unsigned char>& compressed_data, size_t& compressed_size,
                           int* out_position_att_id, int* out_normal_att_id,
                           int* out_texcoord_att_id, int* out_batchid_att_id,
                           const std::vector<float>* batchIds) {
#ifdef HAS_DRACO
    if (!params.enable_compression || !geometry) return false;

    osg::Vec3Array* vertexArray = dynamic_cast<osg::Vec3Array*>(geometry->getVertexArray());
    if (!vertexArray || vertexArray->empty()) return false;

    std::unique_ptr<draco::Mesh> dracoMesh(new draco::Mesh());
    const size_t vertexCount = vertexArray->size();
    dracoMesh->set_num_points(vertexCount);

    // Position attribute
    draco::GeometryAttribute posAttr;
    posAttr.Init(draco::GeometryAttribute::POSITION, nullptr, 3, draco::DT_FLOAT32, false, sizeof(float)*3, 0);
    int posAttId = dracoMesh->AddAttribute(posAttr, true, vertexCount);
    if (out_position_att_id) *out_position_att_id = posAttId;
    for (size_t i = 0; i < vertexCount; ++i) {
        const osg::Vec3& v = vertexArray->at(i);
        float pos[3] = {(float)v.x(), (float)v.y(), (float)v.z()};
        dracoMesh->attribute(posAttId)->SetAttributeValue(draco::AttributeValueIndex(i), &pos[0]);
    }

    // Normal attribute
    osg::Vec3Array* normalArray = dynamic_cast<osg::Vec3Array*>(geometry->getNormalArray());
    if (normalArray && normalArray->size() == vertexCount) {
        draco::GeometryAttribute normAttr;
        normAttr.Init(draco::GeometryAttribute::NORMAL, nullptr, 3, draco::DT_FLOAT32, false, sizeof(float)*3, 0);
        int normAttId = dracoMesh->AddAttribute(normAttr, true, vertexCount);
        if (out_normal_att_id) *out_normal_att_id = normAttId;
        for (size_t i = 0; i < vertexCount; ++i) {
            const osg::Vec3& n = normalArray->at(i);
            float norm[3] = {(float)n.x(), (float)n.y(), (float)n.z()};
            dracoMesh->attribute(normAttId)->SetAttributeValue(draco::AttributeValueIndex(i), &norm[0]);
        }
    }

    // TexCoord attribute
    osg::Vec2Array* texCoordArray = dynamic_cast<osg::Vec2Array*>(geometry->getTexCoordArray(0));
    if (texCoordArray && texCoordArray->size() == vertexCount) {
        draco::GeometryAttribute uvAttr;
        uvAttr.Init(draco::GeometryAttribute::TEX_COORD, nullptr, 2, draco::DT_FLOAT32, false, sizeof(float)*2, 0);
        int uvAttId = dracoMesh->AddAttribute(uvAttr, true, vertexCount);
        if (out_texcoord_att_id) *out_texcoord_att_id = uvAttId;
        for (size_t i = 0; i < vertexCount; ++i) {
            const osg::Vec2& uv = texCoordArray->at(i);
            float tex[2] = {(float)uv.x(), (float)uv.y()};
            dracoMesh->attribute(uvAttId)->SetAttributeValue(draco::AttributeValueIndex(i), &tex[0]);
        }
    }

    // Indices / faces
    if (geometry->getNumPrimitiveSets() > 0) {
        osg::PrimitiveSet* ps = geometry->getPrimitiveSet(0);
        unsigned numIndices = ps->getNumIndices();
        if (numIndices > 0) {
            std::vector<uint32_t> indices(numIndices);
            for (unsigned i = 0; i < numIndices; ++i) indices[i] = ps->index(i);
            size_t faceCount = numIndices / 3;
            dracoMesh->SetNumFaces(faceCount);
            for (size_t i = 0; i < faceCount; ++i) {
                draco::Mesh::Face face;
                face[0] = indices[i*3]; face[1] = indices[i*3+1]; face[2] = indices[i*3+2];
                dracoMesh->SetFace(draco::FaceIndex(i), face);
            }
        }
    }

    draco::Encoder encoder;
    encoder.SetSpeedOptions(5, 5);
    encoder.SetAttributeQuantization(draco::GeometryAttribute::POSITION, params.position_quantization_bits);
    if (normalArray) encoder.SetAttributeQuantization(draco::GeometryAttribute::NORMAL, params.normal_quantization_bits);
    if (texCoordArray) encoder.SetAttributeQuantization(draco::GeometryAttribute::TEX_COORD, params.tex_coord_quantization_bits);

    draco::EncoderBuffer buffer;
    draco::Status status = encoder.EncodeMeshToBuffer(*dracoMesh, &buffer);
    if (!status.ok()) return false;

    compressed_size = buffer.size();
    compressed_data.resize(compressed_size);
    std::memcpy(compressed_data.data(), buffer.data(), compressed_size);
    return true;
#else
    (void)geometry; (void)params; (void)compressed_data; (void)compressed_size;
    (void)out_position_att_id; (void)out_normal_att_id; (void)out_texcoord_att_id; (void)out_batchid_att_id;
    (void)batchIds;
    return false;
#endif
}

// ============================================================
// Texture Processing
// ============================================================
bool process_texture(osg::Texture* tex, std::vector<unsigned char>& image_data,
                     std::string& mime_type, bool enable_texture_compress) {
#ifdef HAS_BASISU
    if (enable_texture_compress && tex && tex->getNumImages() > 0) {
        osg::Image* img = tex->getImage(0);
        if (img) {
            int w = img->s(), h = img->t();
            GLenum fmt = img->getPixelFormat();
            const unsigned char* src = img->data();

            std::vector<unsigned char> rgba(w * h * 4);
            if (fmt == GL_RGBA) {
                rgba.assign(src, src + w * h * 4);
            } else if (fmt == GL_RGB) {
                for (int i = 0; i < w * h; i++) {
                    rgba[i*4]=src[i*3]; rgba[i*4+1]=src[i*3+1]; rgba[i*4+2]=src[i*3+2]; rgba[i*4+3]=255;
                }
            } else if (fmt == GL_BGRA) {
                for (int i = 0; i < w * h; i++) {
                    rgba[i*4]=src[i*4+2]; rgba[i*4+1]=src[i*4+1]; rgba[i*4+2]=src[i*4]; rgba[i*4+3]=src[i*4+3];
                }
            }
            if (!rgba.empty()) {
                std::vector<unsigned char> ktx2;
                if (compress_to_ktx2(rgba, w, h, ktx2)) {
                    image_data = ktx2;
                    mime_type = "image/ktx2";
                    return true;
                }
            }
        }
    }
#else
    (void)enable_texture_compress;
#endif

    // Fallback: pass through raw image data
    if (tex && tex->getNumImages() > 0) {
        osg::Image* img = tex->getImage(0);
        if (img) {
            image_data.assign(img->data(), img->data() + img->getTotalSizeInBytes());
            mime_type = (img->getPixelFormat() == GL_RGB) ? "image/jpeg" : "image/png";
            return true;
        }
    }
    return false;
}
