#ifndef HLOD_OPTIMIZER_LIBRARY
#define TINYGLTF_IMPLEMENTATION
#define TINYGLTF_ENABLE_DRACO
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#endif
#include <tiny_gltf.h>
#ifdef HLOD_OPTIMIZER_LIBRARY
#include <stb_image.h>
#include <stb_image_write.h>
#endif

#include "hlod_optimizer.h"

#include <basisu/transcoder/basisu_transcoder.h>
#include <basisu/encoder/basisu_comp.h>
#include <draco/compression/encode.h>
#include <draco/core/encoder_buffer.h>
#include <draco/mesh/mesh.h>
#include <meshoptimizer.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;

struct Options {
    fs::path input;
    fs::path output;
    int atlases = 16;
    int atlas_size = 512;
    double ratio = 0.05;
    double max_error = 0.25;
    int jpeg_quality = 70;
};

struct Vertex {
    float px = 0, py = 0, pz = 0;
    float nx = 0, ny = 0, nz = 1;
    float u = 0, v = 0;
};

struct ImageRGBA {
    int width = 0;
    int height = 0;
    std::vector<unsigned char> pixels;
};

struct Slot {
    int atlas = 0;
    int column = 0;
    int row = 0;
};

struct Group {
    std::vector<Vertex> vertices;
    std::vector<unsigned int> indices;
};

static void usage() {
    std::cout
        << "Usage: root_glb_optimizer input.glb output.glb [options]\n"
        << "  --atlases N       1..64 (default 16)\n"
        << "  --atlas-size N    128..4096 (default 512)\n"
        << "  --ratio R         0.001..1 (default 0.05)\n"
        << "  --max-error M     absolute position error in model units (default 0.25)\n"
        << "  --jpeg-quality N  1..100 (default 70)\n";
}

static Options parse_options(int argc, char** argv) {
    if (argc < 3) {
        usage();
        throw std::runtime_error("input and output are required");
    }
    Options o;
    o.input = fs::absolute(argv[1]);
    o.output = fs::absolute(argv[2]);
    for (int i = 3; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--atlases" && i + 1 < argc) o.atlases = std::stoi(argv[++i]);
        else if (arg == "--atlas-size" && i + 1 < argc) o.atlas_size = std::stoi(argv[++i]);
        else if (arg == "--ratio" && i + 1 < argc) o.ratio = std::stod(argv[++i]);
        else if (arg == "--max-error" && i + 1 < argc) o.max_error = std::stod(argv[++i]);
        else if (arg == "--jpeg-quality" && i + 1 < argc) o.jpeg_quality = std::stoi(argv[++i]);
        else if (arg == "--help" || arg == "-h") { usage(); std::exit(0); }
        else throw std::runtime_error("unknown or incomplete option: " + arg);
    }
    if (o.input == o.output) throw std::runtime_error("output must differ from input");
    if (!fs::is_regular_file(o.input)) throw std::runtime_error("input does not exist");
    if (fs::exists(o.output)) throw std::runtime_error("output already exists");
    if (o.atlases < 1 || o.atlases > 64) throw std::runtime_error("--atlases must be 1..64");
    if (o.atlas_size < 128 || o.atlas_size > 4096) throw std::runtime_error("--atlas-size must be 128..4096");
    if (!(o.ratio >= 0.001 && o.ratio <= 1.0)) throw std::runtime_error("--ratio must be 0.001..1");
    if (!(o.max_error > 0.0 && o.max_error <= 1000.0)) throw std::runtime_error("--max-error must be >0 and <=1000");
    if (o.jpeg_quality < 1 || o.jpeg_quality > 100) throw std::runtime_error("--jpeg-quality must be 1..100");
    return o;
}

static size_t component_size(int type) {
    switch (type) {
        case TINYGLTF_COMPONENT_TYPE_BYTE:
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE: return 1;
        case TINYGLTF_COMPONENT_TYPE_SHORT:
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: return 2;
        case TINYGLTF_COMPONENT_TYPE_INT:
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
        case TINYGLTF_COMPONENT_TYPE_FLOAT: return 4;
        case TINYGLTF_COMPONENT_TYPE_DOUBLE: return 8;
        default: throw std::runtime_error("unsupported accessor component type");
    }
}

static int component_count(int type) {
    switch (type) {
        case TINYGLTF_TYPE_SCALAR: return 1;
        case TINYGLTF_TYPE_VEC2: return 2;
        case TINYGLTF_TYPE_VEC3: return 3;
        case TINYGLTF_TYPE_VEC4: return 4;
        default: throw std::runtime_error("unsupported accessor shape");
    }
}

static const unsigned char* accessor_element(
    const tinygltf::Model& model, const tinygltf::Accessor& accessor, size_t index) {
    if (accessor.bufferView < 0 || accessor.bufferView >= static_cast<int>(model.bufferViews.size()))
        throw std::runtime_error("accessor has no decoded bufferView");
    const auto& view = model.bufferViews[accessor.bufferView];
    const auto& buffer = model.buffers.at(view.buffer);
    size_t packed = component_size(accessor.componentType) * component_count(accessor.type);
    size_t stride = view.byteStride ? view.byteStride : packed;
    size_t offset = view.byteOffset + accessor.byteOffset + index * stride;
    if (offset + packed > buffer.data.size()) throw std::runtime_error("accessor exceeds buffer");
    return buffer.data.data() + offset;
}

template <typename T>
static T load_scalar(const unsigned char* p) {
    T value;
    std::memcpy(&value, p, sizeof(T));
    return value;
}

static double read_number(const unsigned char* p, int component_type) {
    switch (component_type) {
        case TINYGLTF_COMPONENT_TYPE_BYTE: return load_scalar<int8_t>(p);
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE: return load_scalar<uint8_t>(p);
        case TINYGLTF_COMPONENT_TYPE_SHORT: return load_scalar<int16_t>(p);
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: return load_scalar<uint16_t>(p);
        case TINYGLTF_COMPONENT_TYPE_INT: return load_scalar<int32_t>(p);
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT: return load_scalar<uint32_t>(p);
        case TINYGLTF_COMPONENT_TYPE_FLOAT: return load_scalar<float>(p);
        case TINYGLTF_COMPONENT_TYPE_DOUBLE: return load_scalar<double>(p);
        default: throw std::runtime_error("unsupported numeric component");
    }
}

static std::array<float, 4> read_vector(
    const tinygltf::Model& model, int accessor_index, size_t index) {
    const auto& accessor = model.accessors.at(accessor_index);
    const unsigned char* p = accessor_element(model, accessor, index);
    std::array<float, 4> result = {0, 0, 0, 1};
    size_t step = component_size(accessor.componentType);
    for (int c = 0; c < component_count(accessor.type); ++c)
        result[c] = static_cast<float>(read_number(p + c * step, accessor.componentType));
    return result;
}

static std::vector<unsigned int> read_indices(
    const tinygltf::Model& model, const tinygltf::Primitive& primitive, size_t vertex_count) {
    std::vector<unsigned int> result;
    if (primitive.indices < 0) {
        result.resize(vertex_count);
        for (size_t i = 0; i < vertex_count; ++i) result[i] = static_cast<unsigned int>(i);
        return result;
    }
    const auto& accessor = model.accessors.at(primitive.indices);
    result.resize(accessor.count);
    for (size_t i = 0; i < accessor.count; ++i) {
        const unsigned char* p = accessor_element(model, accessor, i);
        result[i] = static_cast<unsigned int>(read_number(p, accessor.componentType));
    }
    return result;
}

static std::vector<unsigned char> image_bytes(const tinygltf::Model& model, int image_index) {
    const auto& image = model.images.at(image_index);
    if (!image.image.empty()) return image.image;
    if (image.bufferView < 0) throw std::runtime_error("embedded image has no bytes");
    const auto& view = model.bufferViews.at(image.bufferView);
    const auto& buffer = model.buffers.at(view.buffer);
    if (view.byteOffset + view.byteLength > buffer.data.size())
        throw std::runtime_error("image bufferView exceeds buffer");
    return std::vector<unsigned char>(
        buffer.data.begin() + view.byteOffset,
        buffer.data.begin() + view.byteOffset + view.byteLength);
}

static int texture_image(const tinygltf::Texture& texture) {
    auto extension = texture.extensions.find("KHR_texture_basisu");
    if (extension != texture.extensions.end() && extension->second.IsObject()) {
        const auto& object = extension->second.Get<tinygltf::Value::Object>();
        auto source = object.find("source");
        if (source != object.end() && source->second.IsInt()) return source->second.Get<int>();
    }
    return texture.source;
}

static ImageRGBA decode_ktx2(const std::vector<unsigned char>& bytes) {
    basist::ktx2_transcoder transcoder;
    if (!transcoder.init(bytes.data(), static_cast<uint32_t>(bytes.size())))
        throw std::runtime_error("BasisU failed to parse KTX2");
    if (!transcoder.start_transcoding()) throw std::runtime_error("BasisU failed to start KTX2 transcoding");
    basist::ktx2_image_level_info info;
    if (!transcoder.get_image_level_info(info, 0, 0, 0))
        throw std::runtime_error("BasisU failed to query KTX2 level");
    ImageRGBA result;
    result.width = static_cast<int>(info.m_orig_width);
    result.height = static_cast<int>(info.m_orig_height);
    result.pixels.resize(static_cast<size_t>(result.width) * result.height * 4);
    if (!transcoder.transcode_image_level(
            0, 0, 0, result.pixels.data(),
            static_cast<uint32_t>(result.width * result.height),
            basist::transcoder_texture_format::cTFRGBA32))
        throw std::runtime_error("BasisU failed to decode KTX2 to RGBA");
    return result;
}

static ImageRGBA decode_image(const std::vector<unsigned char>& bytes) {
    static const unsigned char ktx2_magic[12] = {
        0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB,
        0x0D, 0x0A, 0x1A, 0x0A
    };
    if (bytes.size() >= sizeof(ktx2_magic) &&
        std::memcmp(bytes.data(), ktx2_magic, sizeof(ktx2_magic)) == 0)
        return decode_ktx2(bytes);

    int width = 0, height = 0, source_channels = 0;
    unsigned char* decoded = stbi_load_from_memory(
        bytes.data(), static_cast<int>(bytes.size()),
        &width, &height, &source_channels, 4);
    if (!decoded) throw std::runtime_error("failed to decode JPEG/PNG texture");
    ImageRGBA result;
    result.width = width;
    result.height = height;
    result.pixels.assign(decoded, decoded + static_cast<size_t>(width) * height * 4);
    stbi_image_free(decoded);
    return result;
}

static void jpeg_callback(void* context, void* data, int size) {
    auto* bytes = static_cast<std::vector<unsigned char>*>(context);
    const auto* begin = static_cast<unsigned char*>(data);
    bytes->insert(bytes->end(), begin, begin + size);
}

static std::vector<unsigned char> encode_jpeg(
    const std::vector<unsigned char>& rgb, int width, int height, int quality) {
    std::vector<unsigned char> result;
    if (!stbi_write_jpg_to_func(jpeg_callback, &result, width, height, 3, rgb.data(), quality))
        throw std::runtime_error("JPEG encoding failed");
    return result;
}

static size_t append_aligned(std::vector<unsigned char>& buffer, const std::vector<unsigned char>& data) {
    while (buffer.size() % 4) buffer.push_back(0);
    size_t offset = buffer.size();
    buffer.insert(buffer.end(), data.begin(), data.end());
    return offset;
}

static std::array<double, 4> material_color(const tinygltf::Material& material) {
    const auto& color = material.pbrMetallicRoughness.baseColorFactor;
    if (color.size() == 4) return {color[0], color[1], color[2], color[3]};
    return {1.0, 1.0, 1.0, 1.0};
}

static std::vector<std::vector<unsigned char>> build_atlas_pixels(
    const tinygltf::Model& model, int atlas_count, int atlas_size,
    int grid) {
    std::vector<std::vector<unsigned char>> pixels(
        atlas_count, std::vector<unsigned char>(static_cast<size_t>(atlas_size) * atlas_size * 3, 96));
    const int cell = atlas_size / grid;
    const int padding = std::max(1, cell / 32);
    const int inner = std::max(1, cell - padding * 2);

    for (size_t material_index = 0; material_index < model.materials.size(); ++material_index) {
        int atlas = static_cast<int>(material_index % atlas_count);
        int local = static_cast<int>(material_index / atlas_count);
        int column = local % grid;
        int row = local / grid;
        if (row >= grid) throw std::runtime_error("atlas slot overflow");

        const auto& material = model.materials[material_index];
        const auto factor = material_color(material);
        ImageRGBA image;
        int texture_index = material.pbrMetallicRoughness.baseColorTexture.index;
        if (texture_index >= 0 && texture_index < static_cast<int>(model.textures.size())) {
            int image_index = texture_image(model.textures[texture_index]);
            if (image_index >= 0 && image_index < static_cast<int>(model.images.size())) {
                image = decode_image(image_bytes(model, image_index));
            }
        }

        for (int y = 0; y < cell; ++y) {
            for (int x = 0; x < cell; ++x) {
                int inner_x = std::clamp(x - padding, 0, inner - 1);
                int inner_y = std::clamp(y - padding, 0, inner - 1);
                unsigned char rgba[4] = {255, 255, 255, 255};
                if (image.width > 0 && image.height > 0) {
                    int sx = inner <= 1 ? 0 : inner_x * (image.width - 1) / (inner - 1);
                    int sy = inner <= 1 ? 0 : inner_y * (image.height - 1) / (inner - 1);
                    size_t source = (static_cast<size_t>(sy) * image.width + sx) * 4;
                    for (int c = 0; c < 4; ++c) rgba[c] = image.pixels[source + c];
                }
                size_t destination =
                    (static_cast<size_t>(row * cell + y) * atlas_size + column * cell + x) * 3;
                for (int c = 0; c < 3; ++c) {
                    double alpha = (rgba[3] / 255.0) * factor[3];
                    double value = rgba[c] * factor[c] * alpha;
                    pixels[atlas][destination + c] = static_cast<unsigned char>(std::clamp(value, 0.0, 255.0));
                }
            }
        }
        if ((material_index + 1) % 250 == 0 || material_index + 1 == model.materials.size())
            std::cout << "  atlas textures: " << (material_index + 1) << "/" << model.materials.size() << "\r" << std::flush;
    }
    std::cout << "\n";

    return pixels;
}

static std::vector<Group> merge_geometry(
    const tinygltf::Model& model, int atlas_count, int atlas_size, int grid) {
    std::vector<Group> groups(atlas_count);
    const int cell = atlas_size / grid;
    const int padding = std::max(1, cell / 32);
    const int inner = std::max(1, cell - padding * 2);
    size_t primitive_count = 0;

    for (const auto& mesh : model.meshes) {
        for (const auto& primitive : mesh.primitives) {
            ++primitive_count;
            if (primitive.mode != TINYGLTF_MODE_TRIANGLES) continue;
            auto position_it = primitive.attributes.find("POSITION");
            if (position_it == primitive.attributes.end()) continue;
            int material_index = primitive.material >= 0 ? primitive.material : 0;
            if (material_index >= static_cast<int>(model.materials.size())) material_index = 0;
            int atlas = material_index % atlas_count;
            int local = material_index / atlas_count;
            int column = local % grid;
            int row = local / grid;

            const auto& position_accessor = model.accessors.at(position_it->second);
            auto normal_it = primitive.attributes.find("NORMAL");
            auto uv_it = primitive.attributes.find("TEXCOORD_0");
            Group& group = groups[atlas];
            unsigned int base = static_cast<unsigned int>(group.vertices.size());
            group.vertices.reserve(group.vertices.size() + position_accessor.count);
            for (size_t i = 0; i < position_accessor.count; ++i) {
                auto p = read_vector(model, position_it->second, i);
                auto n = normal_it == primitive.attributes.end()
                    ? std::array<float, 4>{0, 0, 1, 1}
                    : read_vector(model, normal_it->second, i);
                auto uv = uv_it == primitive.attributes.end()
                    ? std::array<float, 4>{0.5f, 0.5f, 0, 1}
                    : read_vector(model, uv_it->second, i);
                float source_u = std::clamp(uv[0], 0.0f, 1.0f);
                float source_v = std::clamp(uv[1], 0.0f, 1.0f);
                Vertex vertex;
                vertex.px = p[0]; vertex.py = p[1]; vertex.pz = p[2];
                vertex.nx = n[0]; vertex.ny = n[1]; vertex.nz = n[2];
                vertex.u = static_cast<float>(column * cell + padding + source_u * std::max(0, inner - 1) + 0.5) / atlas_size;
                vertex.v = static_cast<float>(row * cell + padding + source_v * std::max(0, inner - 1) + 0.5) / atlas_size;
                group.vertices.push_back(vertex);
            }
            auto source_indices = read_indices(model, primitive, position_accessor.count);
            group.indices.reserve(group.indices.size() + source_indices.size());
            for (unsigned int index : source_indices) group.indices.push_back(base + index);
        }
    }
    std::cout << "  source primitives read: " << primitive_count << "\n";
    return groups;
}

static void simplify_group(Group& group, double ratio, double max_error) {
    if (group.indices.size() < 6 || group.vertices.empty() || ratio >= 0.999) return;
    size_t old_indices = group.indices.size();
    size_t target = static_cast<size_t>(std::floor(old_indices * ratio / 3.0)) * 3;
    target = std::max<size_t>(3, target);
    std::vector<unsigned int> simplified(old_indices);
    float result_error = 0.0f;
    // Attribute-aware topological simplification preserves disconnected
    // components and material/UV seams. LockBorder prevents the real outer
    // boundary of each GLB from moving, so independently processed adjacent
    // HLOD files retain coincident border coordinates. ErrorAbsolute makes
    // the quality bound independent of the physical extent of the GLB.
    const float attribute_weights[5] = {0.10f, 0.10f, 0.10f, 0.05f, 0.05f};
    size_t count = meshopt_simplifyWithAttributes(
        simplified.data(), group.indices.data(), group.indices.size(),
        &group.vertices[0].px, group.vertices.size(), sizeof(Vertex),
        &group.vertices[0].nx, sizeof(Vertex), attribute_weights, 5, nullptr,
        target, static_cast<float>(max_error),
        meshopt_SimplifyLockBorder | meshopt_SimplifyErrorAbsolute |
            meshopt_SimplifyPermissive,
        &result_error);
    if (count >= 3) {
        simplified.resize(count);
        group.indices.swap(simplified);
    }
    meshopt_optimizeVertexCache(group.indices.data(), group.indices.data(),
                                group.indices.size(), group.vertices.size());
    std::vector<Vertex> compact(group.vertices.size());
    size_t vertex_count = meshopt_optimizeVertexFetch(
        compact.data(), group.indices.data(), group.indices.size(),
        group.vertices.data(), group.vertices.size(), sizeof(Vertex));
    compact.resize(vertex_count);
    group.vertices.swap(compact);
}

static void simplify_group_original(Group& group, double ratio) {
    if (group.indices.size() < 6 || group.vertices.empty() || ratio >= 0.999) return;
    const size_t old_indices = group.indices.size();
    size_t target = static_cast<size_t>(old_indices * ratio);
    target = std::max<size_t>(3, target - target % 3);
    std::vector<unsigned int> simplified(old_indices);
    float result_error = 0.0f;
    const float normal_weights[3] = {0.5f, 0.5f, 0.5f};
    size_t count = meshopt_simplifyWithAttributes(
        simplified.data(), group.indices.data(), old_indices,
        &group.vertices[0].px, group.vertices.size(), sizeof(Vertex),
        &group.vertices[0].nx, sizeof(Vertex), normal_weights, 3, nullptr,
        target, 0.01f, 0, &result_error);
    if (count >= 3) {
        simplified.resize(count);
        group.indices.swap(simplified);
    }
    meshopt_optimizeVertexCache(group.indices.data(), group.indices.data(),
                                group.indices.size(), group.vertices.size());
    std::vector<Vertex> compact(group.vertices.size());
    size_t vertex_count = meshopt_optimizeVertexFetch(
        compact.data(), group.indices.data(), group.indices.size(),
        group.vertices.data(), group.vertices.size(), sizeof(Vertex));
    compact.resize(vertex_count);
    group.vertices.swap(compact);
}

struct DracoResult {
    std::vector<unsigned char> bytes;
    int position_id = -1;
    int normal_id = -1;
    int uv_id = -1;
};

static DracoResult encode_draco(const Group& group, int position_bits,
                                int normal_bits, int uv_bits) {
    draco::Mesh mesh;
    mesh.set_num_points(static_cast<uint32_t>(group.vertices.size()));

    auto add_attribute = [&](draco::GeometryAttribute::Type type, int components,
                             size_t offset) -> int {
        draco::GeometryAttribute attribute;
        attribute.Init(type, nullptr, components, draco::DT_FLOAT32, false,
                       components * sizeof(float), 0);
        int id = mesh.AddAttribute(attribute, true, group.vertices.size());
        for (size_t i = 0; i < group.vertices.size(); ++i) {
            const unsigned char* vertex = reinterpret_cast<const unsigned char*>(&group.vertices[i]);
            mesh.attribute(id)->SetAttributeValue(draco::AttributeValueIndex(static_cast<uint32_t>(i)), vertex + offset);
        }
        return mesh.attribute(id)->unique_id();
    };

    DracoResult result;
    result.position_id = add_attribute(draco::GeometryAttribute::POSITION, 3, offsetof(Vertex, px));
    result.normal_id = add_attribute(draco::GeometryAttribute::NORMAL, 3, offsetof(Vertex, nx));
    result.uv_id = add_attribute(draco::GeometryAttribute::TEX_COORD, 2, offsetof(Vertex, u));
    mesh.SetNumFaces(group.indices.size() / 3);
    for (size_t i = 0; i + 2 < group.indices.size(); i += 3) {
        draco::Mesh::Face face;
        face[0] = group.indices[i]; face[1] = group.indices[i + 1]; face[2] = group.indices[i + 2];
        mesh.SetFace(draco::FaceIndex(static_cast<uint32_t>(i / 3)), face);
    }

    draco::Encoder encoder;
    encoder.SetSpeedOptions(5, 5);
    encoder.SetAttributeQuantization(draco::GeometryAttribute::POSITION, position_bits);
    encoder.SetAttributeQuantization(draco::GeometryAttribute::NORMAL, normal_bits);
    encoder.SetAttributeQuantization(draco::GeometryAttribute::TEX_COORD, uv_bits);
    draco::EncoderBuffer encoded;
    auto status = encoder.EncodeMeshToBuffer(mesh, &encoded);
    if (!status.ok())
        throw std::runtime_error(std::string("Draco encoding failed: ") + status.error_msg());
    result.bytes.assign(encoded.data(), encoded.data() + encoded.size());
    return result;
}

static int add_accessor(tinygltf::Model& model, int type, int component_type,
                        size_t count, const std::vector<double>& minimum = {},
                        const std::vector<double>& maximum = {}) {
    tinygltf::Accessor accessor;
    accessor.bufferView = -1;
    accessor.type = type;
    accessor.componentType = component_type;
    accessor.count = count;
    accessor.minValues = minimum;
    accessor.maxValues = maximum;
    model.accessors.push_back(accessor);
    return static_cast<int>(model.accessors.size() - 1);
}

static std::pair<std::vector<double>, std::vector<double>> bounds(const Group& group) {
    std::vector<double> minimum(3, std::numeric_limits<double>::max());
    std::vector<double> maximum(3, std::numeric_limits<double>::lowest());
    for (const auto& v : group.vertices) {
        double p[3] = {v.px, v.py, v.pz};
        for (int c = 0; c < 3; ++c) {
            minimum[c] = std::min(minimum[c], p[c]);
            maximum[c] = std::max(maximum[c], p[c]);
        }
    }
    return {minimum, maximum};
}

static std::vector<unsigned char> encode_ktx2(
    const std::vector<unsigned char>& rgb, int width, int height, int quality) {
    basisu::basisu_encoder_init(false, false);
    std::vector<unsigned char> rgba(static_cast<size_t>(width) * height * 4);
    for (size_t i = 0; i < static_cast<size_t>(width) * height; ++i) {
        rgba[i * 4] = rgb[i * 3]; rgba[i * 4 + 1] = rgb[i * 3 + 1];
        rgba[i * 4 + 2] = rgb[i * 3 + 2]; rgba[i * 4 + 3] = 255;
    }
    basisu::vector<basisu::image> images;
    images.push_back(basisu::image(rgba.data(), width, height, 4));
    size_t size = 0;
    void* encoded = basisu::basis_compress(
        basist::basis_tex_format::cETC1S, images,
        static_cast<unsigned int>(quality) | basisu::cFlagKTX2 | basisu::cFlagGenMipsWrap,
        1.0f, &size, nullptr);
    if (!encoded || !size) throw std::runtime_error("KTX2 atlas encoding failed");
    std::vector<unsigned char> result(static_cast<unsigned char*>(encoded),
                                      static_cast<unsigned char*>(encoded) + size);
    basisu::basis_free_data(encoded);
    return result;
}

static tinygltf::Model build_output(
    std::vector<Group>& groups,
    const std::vector<std::vector<unsigned char>>& atlas_pixels,
    int atlas_size, bool texture_compress, int ktx2_quality,
    int position_bits, int normal_bits, int uv_bits) {
    tinygltf::Model model;
    model.asset.version = "2.0";
    model.asset.generator = "standalone-root-glb-optimizer";
    model.extensionsUsed = {"KHR_draco_mesh_compression", "KHR_materials_unlit"};
    model.extensionsRequired = {"KHR_draco_mesh_compression"};
    tinygltf::Buffer binary;
    binary.name = "optimized-root";

    tinygltf::Sampler sampler;
    sampler.minFilter = TINYGLTF_TEXTURE_FILTER_LINEAR_MIPMAP_LINEAR;
    sampler.magFilter = TINYGLTF_TEXTURE_FILTER_LINEAR;
    sampler.wrapS = TINYGLTF_TEXTURE_WRAP_CLAMP_TO_EDGE;
    sampler.wrapT = TINYGLTF_TEXTURE_WRAP_CLAMP_TO_EDGE;
    model.samplers.push_back(sampler);

    for (size_t i = 0; i < atlas_pixels.size(); ++i) {
        std::vector<unsigned char> encoded = texture_compress
            ? encode_ktx2(atlas_pixels[i], atlas_size, atlas_size, ktx2_quality)
            : encode_jpeg(atlas_pixels[i], atlas_size, atlas_size, 80);
        size_t offset = append_aligned(binary.data, encoded);
        tinygltf::BufferView view;
        view.buffer = 0; view.byteOffset = offset; view.byteLength = encoded.size();
        model.bufferViews.push_back(view);
        tinygltf::Image image;
        image.name = "atlas_" + std::to_string(i);
        image.mimeType = texture_compress ? "image/ktx2" : "image/jpeg";
        image.bufferView = static_cast<int>(model.bufferViews.size() - 1);
        image.width = atlas_size; image.height = atlas_size; image.component = 3; image.bits = 8;
        model.images.push_back(image);
        tinygltf::Texture texture;
        texture.name = image.name;
        if (texture_compress) {
            tinygltf::Value::Object basisu;
            basisu["source"] = tinygltf::Value(static_cast<int>(i));
            texture.extensions["KHR_texture_basisu"] = tinygltf::Value(basisu);
        } else {
            texture.source = static_cast<int>(i);
        }
        texture.sampler = 0;
        model.textures.push_back(texture);
        tinygltf::Material material;
        material.name = "atlas_material_" + std::to_string(i);
        material.pbrMetallicRoughness.baseColorTexture.index = static_cast<int>(i);
        material.pbrMetallicRoughness.metallicFactor = 0.0;
        material.pbrMetallicRoughness.roughnessFactor = 1.0;
        material.doubleSided = true;
        material.extensions["KHR_materials_unlit"] = tinygltf::Value(tinygltf::Value::Object{});
        model.materials.push_back(material);
    }

    tinygltf::Mesh mesh;
    mesh.name = "optimized-root";
    for (size_t group_index = 0; group_index < groups.size(); ++group_index) {
        const Group& group = groups[group_index];
        if (group.indices.empty() || group.vertices.empty()) continue;
        DracoResult draco = encode_draco(group, position_bits, normal_bits, uv_bits);
        size_t offset = append_aligned(binary.data, draco.bytes);
        tinygltf::BufferView view;
        view.buffer = 0; view.byteOffset = offset; view.byteLength = draco.bytes.size();
        model.bufferViews.push_back(view);
        int view_index = static_cast<int>(model.bufferViews.size() - 1);
        auto box = bounds(group);

        tinygltf::Primitive primitive;
        primitive.mode = TINYGLTF_MODE_TRIANGLES;
        primitive.material = static_cast<int>(group_index);
        primitive.indices = add_accessor(model, TINYGLTF_TYPE_SCALAR,
            TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT, group.indices.size());
        primitive.attributes["POSITION"] = add_accessor(model, TINYGLTF_TYPE_VEC3,
            TINYGLTF_COMPONENT_TYPE_FLOAT, group.vertices.size(), box.first, box.second);
        primitive.attributes["NORMAL"] = add_accessor(model, TINYGLTF_TYPE_VEC3,
            TINYGLTF_COMPONENT_TYPE_FLOAT, group.vertices.size());
        primitive.attributes["TEXCOORD_0"] = add_accessor(model, TINYGLTF_TYPE_VEC2,
            TINYGLTF_COMPONENT_TYPE_FLOAT, group.vertices.size());

        tinygltf::Value::Object attributes;
        attributes["POSITION"] = tinygltf::Value(draco.position_id);
        attributes["NORMAL"] = tinygltf::Value(draco.normal_id);
        attributes["TEXCOORD_0"] = tinygltf::Value(draco.uv_id);
        tinygltf::Value::Object extension;
        extension["bufferView"] = tinygltf::Value(view_index);
        extension["attributes"] = tinygltf::Value(attributes);
        primitive.extensions["KHR_draco_mesh_compression"] = tinygltf::Value(extension);
        mesh.primitives.push_back(primitive);
    }
    model.meshes.push_back(mesh);
    tinygltf::Node node;
    node.mesh = 0;
    model.nodes.push_back(node);
    tinygltf::Scene scene;
    scene.nodes.push_back(0);
    model.scenes.push_back(scene);
    model.defaultScene = 0;
    model.buffers.push_back(std::move(binary));
    if (texture_compress) {
        model.extensionsUsed.push_back("KHR_texture_basisu");
        model.extensionsRequired.push_back("KHR_texture_basisu");
    }
    return model;
}

bool optimize_hlod_glb_buffer(const std::string& input_glb,
                              std::string& output_glb,
                              int atlas_cell_size,
                              double ratio,
                              bool enable_texture_compress,
                              int ktx2_quality,
                              int draco_position_bits,
                              int draco_normal_bits,
                              int draco_uv_bits,
                              std::string* error_message) {
    try {
        basist::basisu_transcoder_init();
        tinygltf::TinyGLTF loader;
        loader.SetImagesAsIs(true);
        tinygltf::Model input;
        std::string error, warning;
        const auto* bytes = reinterpret_cast<const unsigned char*>(input_glb.data());
        if (!loader.LoadBinaryFromMemory(&input, &error, &warning, bytes,
                                         static_cast<unsigned int>(input_glb.size())))
            throw std::runtime_error("GLB load failed: " + error);
        if (input.materials.empty()) throw std::runtime_error("input has no materials");

        const int atlas_count = 1;
        const int grid = static_cast<int>(std::ceil(std::sqrt(
            static_cast<double>(input.materials.size()))));
        const int atlas_size = std::min(4096, std::max(128, atlas_cell_size * grid));
        if (atlas_size / grid < 4)
            throw std::runtime_error("atlas is too small for the material count");

        auto atlases = build_atlas_pixels(input, atlas_count, atlas_size, grid);
        auto groups = merge_geometry(input, atlas_count, atlas_size, grid);
        for (auto& group : groups) simplify_group_original(group, ratio);
        tinygltf::Model output = build_output(
            groups, atlases, atlas_size, enable_texture_compress, ktx2_quality,
            draco_position_bits, draco_normal_bits, draco_uv_bits);
        if (output.meshes.empty() || output.meshes[0].primitives.size() != 1)
            throw std::runtime_error("optimizer did not produce exactly one primitive");

        tinygltf::TinyGLTF writer;
        std::ostringstream stream;
        if (!writer.WriteGltfSceneToStream(&output, stream, false, true))
            throw std::runtime_error("failed to serialize optimized GLB");
        output_glb = stream.str();
        return true;
    } catch (const std::exception& e) {
        output_glb.clear();
        if (error_message) *error_message = e.what();
        return false;
    }
}

#ifndef HLOD_OPTIMIZER_LIBRARY
int main(int argc, char** argv) {
    try {
        Options options = parse_options(argc, argv);
        if (options.atlases == 1) {
            std::ifstream input_stream(options.input, std::ios::binary);
            std::string input_bytes((std::istreambuf_iterator<char>(input_stream)),
                                    std::istreambuf_iterator<char>());
            std::string output_bytes;
            std::string optimize_error;
            if (!optimize_hlod_glb_buffer(
                    input_bytes, output_bytes, options.atlas_size, options.ratio,
                    false, 128, 20, 8, 10, &optimize_error))
                throw std::runtime_error(optimize_error);
            fs::create_directories(options.output.parent_path());
            std::ofstream output_stream(options.output, std::ios::binary);
            output_stream.write(output_bytes.data(),
                                static_cast<std::streamsize>(output_bytes.size()));
            if (!output_stream) throw std::runtime_error("failed to write output GLB");
            std::cout << "Output: primitives=1 size="
                      << output_bytes.size() / (1024.0 * 1024.0) << " MiB\n";
            return 0;
        }
        basist::basisu_transcoder_init();

        tinygltf::TinyGLTF loader;
        loader.SetImagesAsIs(true);
        tinygltf::Model input;
        std::string error, warning;
        std::cout << "Loading and decoding input GLB...\n";
        if (!loader.LoadBinaryFromFile(&input, &error, &warning, options.input.string()))
            throw std::runtime_error("GLB load failed: " + error);
        if (!warning.empty()) std::cerr << warning;
        if (input.materials.empty()) throw std::runtime_error("input has no materials");

        int atlas_count = std::min<int>(options.atlases, input.materials.size());
        int slots_per_atlas = static_cast<int>((input.materials.size() + atlas_count - 1) / atlas_count);
        int grid = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(slots_per_atlas))));
        if (options.atlas_size / grid < 4)
            throw std::runtime_error("atlas size is too small for the material count");
        std::cout << "Input: primitives=";
        size_t source_primitives = 0;
        for (const auto& mesh : input.meshes) source_primitives += mesh.primitives.size();
        std::cout << source_primitives << " materials=" << input.materials.size()
                  << " textures=" << input.textures.size() << "\n";
        std::cout << "Building " << atlas_count << " atlases, grid " << grid << "x" << grid << "...\n";
        auto atlas_pixels = build_atlas_pixels(input, atlas_count, options.atlas_size, grid);

        std::cout << "Merging decoded geometry by atlas...\n";
        auto groups = merge_geometry(input, atlas_count, options.atlas_size, grid);
        size_t before = 0, after = 0;
        for (auto& group : groups) {
            before += group.indices.size();
            simplify_group(group, options.ratio, options.max_error);
            after += group.indices.size();
        }
        std::cout << "Geometry indices: " << before << " -> " << after
                  << " (" << (before ? 100.0 * after / before : 0.0) << "%)\n";

        std::cout << "Encoding merged groups with Draco...\n";
        tinygltf::Model output = build_output(
            groups, atlas_pixels, options.atlas_size, false, 128, 20, 8, 10);
        tinygltf::TinyGLTF writer;
        fs::create_directories(options.output.parent_path());
        fs::path temporary = options.output;
        temporary += ".tmp";
        if (fs::exists(temporary)) fs::remove(temporary);
        if (!writer.WriteGltfSceneToFile(&output, temporary.string(), true, true, false, true))
            throw std::runtime_error("failed to write output GLB");
        fs::rename(temporary, options.output);
        double mib = fs::file_size(options.output) / (1024.0 * 1024.0);
        std::cout << "Output: primitives=" << output.meshes[0].primitives.size()
                  << " materials=" << output.materials.size()
                  << " textures=" << output.textures.size()
                  << " size=" << mib << " MiB\n";
        std::cout << options.output.string() << "\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << "\n";
        return 1;
    }
}
#endif
