#include "mesh_processor.h"
#include <cstddef>
#include <osg/Texture>
#include <osg/Image>
#include <osg/Array>
#include <vector>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <cmath>
#include <algorithm>

// ============================================================
// Required dependencies (unconditional — must be available)
// ============================================================
#include <basisu/encoder/basisu_comp.h>
#include <basisu/transcoder/basisu_transcoder.h>
#include <meshoptimizer.h>
#include "draco/compression/encode.h"
#include "draco/core/encoder_buffer.h"
#include "draco/mesh/mesh.h"

// stb_image implementation (needed by tinygltf)
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

// stb_image_write implementation provided by osg_gltf_converter.cpp (TINYGLTF_IMPLEMENTATION)
#include <stb_image_write.h>

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
    try {
        if (rgba_data.empty() || width <= 0 || height <= 0) return false;

        static std::once_flag basisu_init;
        std::call_once(basisu_init, []() { basisu::basisu_encoder_init(); });

        basisu::vector<basisu::image> source_images;
        source_images.push_back(basisu::image(rgba_data.data(), width, height, 4));
        int quality_level = 128;
        std::size_t file_size = 0;

        // NOTE: cFlagThreaded is intentionally NOT set here.
        // When tiles are already processed in parallel (Phase 2), enabling
        // basisu's internal threads would cause N×M thread oversubscription.
        unsigned int flags = quality_level | basisu::cFlagKTX2 | basisu::cFlagGenMipsWrap;
        void* pKTX2_data = basisu::basis_compress(
            basist::basis_tex_format::cETC1S, source_images, flags, 1.0f, &file_size, nullptr);

        ktx2_data.assign((unsigned char*)pKTX2_data, (unsigned char*)pKTX2_data + file_size);
        basisu::basis_free_data(pKTX2_data);
        return true;
    } catch (...) { return false; }
}

// ============================================================
// Mesh Simplification (requires meshoptimizer)
// ============================================================
bool optimize_and_simplify_mesh(
    std::vector<VertexData>& vertices, size_t& vertex_count,
    std::vector<unsigned int>& indices, size_t original_index_count,
    std::vector<unsigned int>& simplified_indices, size_t& simplified_index_count,
    const SimplificationParams& params) {
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
}

bool simplify_mesh_geometry(osg::Geometry* geometry, const SimplificationParams& params) {
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
}

// ============================================================
// Draco Compression
// ============================================================
bool compress_mesh_geometry(osg::Geometry* geometry, const DracoCompressionParams& params,
                           std::vector<unsigned char>& compressed_data, size_t& compressed_size,
                           int* out_position_att_id, int* out_normal_att_id,
                           int* out_texcoord_att_id, int* out_batchid_att_id,
                           const std::vector<float>* batchIds) {
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
}

// ============================================================
// Meshopt Geometry Compression (EXT_meshopt_compression)
// ============================================================
// Uses meshopt_encodeVertexBuffer / meshopt_encodeIndexBuffer
// to losslessly compress float vertex/attribute data and uint32 index data.
// Accessor formats stay standard (FLOAT/VEC3 etc.) — the extension tells
// the decoder to decompress the bufferViews transparently.

bool compress_mesh_geometry_meshopt(osg::Geometry* geometry,
                                    const MeshoptCompressionParams& params,
                                    MeshoptCompressionResult& result) {
    result = MeshoptCompressionResult{};
    if (!params.enable_compression || !geometry) return false;

    osg::Vec3Array* vertexArray = dynamic_cast<osg::Vec3Array*>(geometry->getVertexArray());
    if (!vertexArray || vertexArray->empty()) return false;

    const size_t vertexCount = vertexArray->size();
    result.vertex_count = vertexCount;

    // Extract indices
    std::vector<uint32_t> indices;
    if (geometry->getNumPrimitiveSets() > 0) {
        osg::PrimitiveSet* ps = geometry->getPrimitiveSet(0);
        unsigned numIndices = ps->getNumIndices();
        if (numIndices > 0) {
            indices.resize(numIndices);
            for (unsigned i = 0; i < numIndices; ++i) indices[i] = ps->index(i);
        }
    }
    result.index_count = indices.size();

    // Check for optional attributes
    osg::Vec3Array* normalArray = dynamic_cast<osg::Vec3Array*>(geometry->getNormalArray());
    bool hasNormals = normalArray && normalArray->size() == vertexCount;
    osg::Vec2Array* texCoordArray = dynamic_cast<osg::Vec2Array*>(geometry->getTexCoordArray(0));
    bool hasTexCoords = texCoordArray && texCoordArray->size() == vertexCount;

    // ================================================================
    // Stream 0: Position — raw float32×3, byteStride=12
    // ================================================================
    {
        int byteStride = 3 * static_cast<int>(sizeof(float));
        std::vector<float> raw(vertexCount * 3);
        for (size_t i = 0; i < vertexCount; ++i) {
            osg::Vec3f v = vertexArray->at(i);
            raw[i * 3 + 0] = v.x();
            raw[i * 3 + 1] = v.y();
            raw[i * 3 + 2] = v.z();
        }

        std::vector<unsigned char> encoded(meshopt_encodeVertexBufferBound(vertexCount, byteStride));
        size_t encodedSize = meshopt_encodeVertexBuffer(encoded.data(), encoded.size(),
            raw.data(), vertexCount, byteStride);
        encoded.resize(encodedSize);

        MeshoptCompressionResult::AttrStream stream;
        stream.data = std::move(encoded);
        stream.component_count = 3;
        stream.byte_stride = byteStride;
        stream.filter = "NONE";
        result.attr_streams.push_back(std::move(stream));
    }

    // ================================================================
    // Stream 1: Normal — raw float32×3, byteStride=12
    // ================================================================
    if (hasNormals) {
        int byteStride = 3 * static_cast<int>(sizeof(float));
        std::vector<float> raw(vertexCount * 3);
        for (size_t i = 0; i < vertexCount; ++i) {
            osg::Vec3f n = normalArray->at(i);
            float len = n.length();
            if (len < 0.0001f) { n.set(0, 0, 1); }
            else if (std::abs(len - 1.0f) > 0.0001f) { n.normalize(); }
            raw[i * 3 + 0] = n.x();
            raw[i * 3 + 1] = n.y();
            raw[i * 3 + 2] = n.z();
        }

        std::vector<unsigned char> encoded(meshopt_encodeVertexBufferBound(vertexCount, byteStride));
        size_t encodedSize = meshopt_encodeVertexBuffer(encoded.data(), encoded.size(),
            raw.data(), vertexCount, byteStride);
        encoded.resize(encodedSize);

        MeshoptCompressionResult::AttrStream stream;
        stream.data = std::move(encoded);
        stream.component_count = 3;
        stream.byte_stride = byteStride;
        stream.filter = "NONE";
        result.attr_streams.push_back(std::move(stream));
    }

    // ================================================================
    // Stream 2: TexCoord — raw float32×2, byteStride=8
    // ================================================================
    if (hasTexCoords) {
        int byteStride = 2 * static_cast<int>(sizeof(float));
        std::vector<float> raw(vertexCount * 2);
        for (size_t i = 0; i < vertexCount; ++i) {
            osg::Vec2f uv = texCoordArray->at(i);
            raw[i * 2 + 0] = uv.x();
            raw[i * 2 + 1] = uv.y();
        }

        std::vector<unsigned char> encoded(meshopt_encodeVertexBufferBound(vertexCount, byteStride));
        size_t encodedSize = meshopt_encodeVertexBuffer(encoded.data(), encoded.size(),
            raw.data(), vertexCount, byteStride);
        encoded.resize(encodedSize);

        MeshoptCompressionResult::AttrStream stream;
        stream.data = std::move(encoded);
        stream.component_count = 2;
        stream.byte_stride = byteStride;
        stream.filter = "NONE";
        result.attr_streams.push_back(std::move(stream));
    }

    // ================================================================
    // Index stream — delta + entropy encode
    // ================================================================
    if (!indices.empty()) {
        std::vector<unsigned char> encoded(meshopt_encodeIndexBufferBound(indices.size(), vertexCount));
        size_t encodedSize = meshopt_encodeIndexBuffer(encoded.data(), encoded.size(),
            indices.data(), indices.size());
        encoded.resize(encodedSize);
        result.index_data = std::move(encoded);
    }

    result.success = true;
    return true;
}

// ============================================================
// Texture Processing
// ============================================================
bool process_texture(osg::Texture* tex, std::vector<unsigned char>& image_data,
                     std::string& mime_type, bool enable_texture_compress) {
    // KTX2 path (Basis Universal)
    if (enable_texture_compress && tex && tex->getNumImages() > 0) {
        osg::Image* img = tex->getImage(0);
        if (img) {
            int w = img->s(), h = img->t();
            GLenum fmt = img->getPixelFormat();
            const unsigned char* src = img->data();
            unsigned rowStep = img->getRowStepInBytes();
            unsigned rowSize = img->getRowSizeInBytes();
            bool hasRowPadding = (rowStep != rowSize);

            std::vector<unsigned char> rgba(w * h * 4);
            if (fmt == GL_RGBA) {
                if (hasRowPadding) {
                    for (int row = 0; row < h; row++)
                        std::memcpy(&rgba[row * w * 4], &src[row * rowStep], w * 4);
                } else {
                    rgba.assign(src, src + w * h * 4);
                }
            } else if (fmt == GL_RGB) {
                if (hasRowPadding) {
                    for (int row = 0; row < h; row++) {
                        for (int col = 0; col < w; col++) {
                            int si = row * rowStep + col * 3;
                            int di = (row * w + col) * 4;
                            rgba[di]=src[si]; rgba[di+1]=src[si+1]; rgba[di+2]=src[si+2]; rgba[di+3]=255;
                        }
                    }
                } else {
                    for (int i = 0; i < w * h; i++) {
                        rgba[i*4]=src[i*3]; rgba[i*4+1]=src[i*3+1]; rgba[i*4+2]=src[i*3+2]; rgba[i*4+3]=255;
                    }
                }
            } else if (fmt == GL_BGRA) {
                if (hasRowPadding) {
                    for (int row = 0; row < h; row++) {
                        for (int col = 0; col < w; col++) {
                            int si = row * rowStep + col * 4;
                            int di = (row * w + col) * 4;
                            rgba[di]=src[si+2]; rgba[di+1]=src[si+1]; rgba[di+2]=src[si]; rgba[di+3]=src[si+3];
                        }
                    }
                } else {
                    for (int i = 0; i < w * h; i++) {
                        rgba[i*4]=src[i*4+2]; rgba[i*4+1]=src[i*4+1]; rgba[i*4+2]=src[i*4]; rgba[i*4+3]=src[i*4+3];
                    }
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

    // Fallback: JPEG encode via stb
    if (tex && tex->getNumImages() > 0) {
        osg::Image* img = tex->getImage(0);
        if (img) {
            int w = img->s(), h = img->t();
            GLenum fmt = img->getPixelFormat();
            const unsigned char* src = img->data();
            unsigned rowStep = img->getRowStepInBytes();
            unsigned rowSize = img->getRowSizeInBytes();
            bool hasRowPadding = (rowStep != rowSize);

            // Convert to tightly-packed RGB for JPEG encoding
            std::vector<unsigned char> rgb(w * h * 3);
            switch (fmt) {
            case GL_RGBA:
                if (hasRowPadding) {
                    for (int row = 0; row < h; row++)
                        for (int col = 0; col < w; col++) {
                            int si = row * rowStep + col * 4;
                            int di = (row * w + col) * 3;
                            rgb[di]=src[si]; rgb[di+1]=src[si+1]; rgb[di+2]=src[si+2];
                        }
                } else {
                    for (int i = 0; i < w * h; i++) {
                        rgb[i*3]=src[i*4]; rgb[i*3+1]=src[i*4+1]; rgb[i*3+2]=src[i*4+2];
                    }
                }
                break;
            case GL_BGRA:
                if (hasRowPadding) {
                    for (int row = 0; row < h; row++)
                        for (int col = 0; col < w; col++) {
                            int si = row * rowStep + col * 4;
                            int di = (row * w + col) * 3;
                            rgb[di]=src[si+2]; rgb[di+1]=src[si+1]; rgb[di+2]=src[si];
                        }
                } else {
                    for (int i = 0; i < w * h; i++) {
                        rgb[i*3]=src[i*4+2]; rgb[i*3+1]=src[i*4+1]; rgb[i*3+2]=src[i*4];
                    }
                }
                break;
            case GL_RGB:
                if (hasRowPadding) {
                    for (int row = 0; row < h; row++)
                        std::memcpy(&rgb[row * w * 3], &src[row * rowStep], w * 3);
                } else {
                    rgb.assign(src, src + w * h * 3);
                }
                break;
            default:
                break;
            }

            if (!rgb.empty()) {
                std::vector<char> buf;
                stbi_write_jpg_to_func(write_buf_callback, &buf, w, h, 3, rgb.data(), 80);
                if (!buf.empty()) {
                    image_data.assign(buf.begin(), buf.end());
                    mime_type = "image/jpeg";
                    return true;
                }
            }
        }
    }
    return false;
}
