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

struct ComponentRange {
    size_t vertex_offset = 0;
    size_t vertex_count = 0;
    size_t index_offset = 0;
    size_t index_count = 0;
};

struct Group {
    std::vector<Vertex> vertices;
    std::vector<unsigned int> indices;
    // Source primitives remain disconnected after the atlas merge. Keeping
    // their ranges lets HLOD simplification allocate detail to every source
    // component instead of allowing a few large meshes to consume the entire
    // triangle budget.
    std::vector<ComponentRange> components;
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
            const size_t index_offset = group.indices.size();
            group.indices.reserve(group.indices.size() + source_indices.size());
            for (unsigned int index : source_indices) group.indices.push_back(base + index);
            group.components.push_back({
                base, position_accessor.count, index_offset, source_indices.size()});
        }
    }
    std::cout << "  source primitives read: " << primitive_count << "\n";
    return groups;
}

struct SurfaceProxy {
    Group group;
    std::vector<unsigned char> texture;
    int texture_size = 0;
    double effective_spacing = 0.0;
};

static std::array<unsigned char, 3> sample_rgb(
    const std::vector<unsigned char>& image, int size, float u, float v) {
    const double x = std::clamp(static_cast<double>(u), 0.0, 1.0) * (size - 1);
    const double y = std::clamp(static_cast<double>(v), 0.0, 1.0) * (size - 1);
    const int x0 = static_cast<int>(std::floor(x));
    const int y0 = static_cast<int>(std::floor(y));
    const int x1 = std::min(size - 1, x0 + 1);
    const int y1 = std::min(size - 1, y0 + 1);
    const double tx = x - x0;
    const double ty = y - y0;
    std::array<unsigned char, 3> result{};
    for (int channel = 0; channel < 3; ++channel) {
        const auto value = [&](int px, int py) {
            return static_cast<double>(image[
                (static_cast<size_t>(py) * size + px) * 3 + channel]);
        };
        const double top = value(x0, y0) * (1.0 - tx) + value(x1, y0) * tx;
        const double bottom = value(x0, y1) * (1.0 - tx) + value(x1, y1) * tx;
        result[channel] = static_cast<unsigned char>(std::clamp(
            top * (1.0 - ty) + bottom * ty, 0.0, 255.0));
    }
    return result;
}

// Reconstruct a stable far-distance proxy as a 2.5D top surface. Source
// triangles are rasterized from +Z, the highest hit wins, and its source atlas
// colour is baked into a new texture. This replaces topology decimation for
// HLODs: grid spacing is controlled by a spatial error instead of a triangle
// ratio, so disconnected photogrammetry fragments cannot consume one another's
// simplification budget.
static SurfaceProxy reconstruct_top_surface(
    const Group& source, const std::vector<unsigned char>& source_atlas,
    int source_atlas_size, int requested_max_dimension,
    double requested_spacing) {
    if (source.vertices.empty() || source.indices.size() < 3)
        throw std::runtime_error("surface reconstruction input is empty");
    if (source_atlas_size <= 0 || source_atlas.size() !=
            static_cast<size_t>(source_atlas_size) * source_atlas_size * 3)
        throw std::runtime_error("surface reconstruction atlas is invalid");

    float min_x = source.vertices[0].px;
    float max_x = min_x;
    float min_y = source.vertices[0].py;
    float max_y = min_y;
    for (const Vertex& vertex : source.vertices) {
        min_x = std::min(min_x, vertex.px);
        max_x = std::max(max_x, vertex.px);
        min_y = std::min(min_y, vertex.py);
        max_y = std::max(max_y, vertex.py);
    }
    const double source_width = std::max(1e-6, static_cast<double>(max_x - min_x));
    const double source_height = std::max(1e-6, static_cast<double>(max_y - min_y));
    const int max_dimension = requested_max_dimension > 0
        ? std::clamp(requested_max_dimension, 128, 1024)
        : 1024;

    // All independently reconstructed tiles at a level must use the same XY
    // lattice.  Deriving step_x/step_y from each tile's slightly different
    // geometry bounds made neighbouring boundary vertices miss one another,
    // which showed up as cracks in L0/L1.  Use a square, world-anchored grid.
    // When the requested spacing does not fit the texture budget, grow it by
    // powers of two so normal full-sized siblings still choose the same step.
    const double base_spacing = std::max(1e-6, requested_spacing);
    const double minimum_spacing = std::max(source_width, source_height) /
                                   std::max(1, max_dimension - 3);
    double spacing_multiplier = 1.0;
    while (base_spacing * spacing_multiplier < minimum_spacing)
        spacing_multiplier *= 2.0;
    double spacing = base_spacing * spacing_multiplier;

    double grid_min_x = 0.0;
    double grid_min_y = 0.0;
    int columns = 0;
    int rows = 0;
    for (;;) {
        grid_min_x = std::floor(static_cast<double>(min_x) / spacing) * spacing;
        grid_min_y = std::floor(static_cast<double>(min_y) / spacing) * spacing;
        const double grid_max_x = std::ceil(static_cast<double>(max_x) / spacing) * spacing;
        const double grid_max_y = std::ceil(static_cast<double>(max_y) / spacing) * spacing;
        columns = static_cast<int>(std::llround((grid_max_x - grid_min_x) / spacing)) + 1;
        rows = static_cast<int>(std::llround((grid_max_y - grid_min_y) / spacing)) + 1;
        if (columns <= max_dimension && rows <= max_dimension) break;
        spacing *= 2.0;
    }
    columns = std::max(2, columns);
    rows = std::max(2, rows);
    const double step_x = spacing;
    const double step_y = spacing;
    const size_t sample_count = static_cast<size_t>(columns) * rows;
    const float missing = -std::numeric_limits<float>::infinity();
    std::vector<float> heights(sample_count, missing);
    std::vector<unsigned char> colours(sample_count * 3, 96);

    auto sample_index = [columns](int x, int y) {
        return static_cast<size_t>(y) * columns + x;
    };

    size_t rasterized_triangles = 0;
    for (size_t triangle = 0; triangle + 2 < source.indices.size(); triangle += 3) {
        const unsigned int ia = source.indices[triangle];
        const unsigned int ib = source.indices[triangle + 1];
        const unsigned int ic = source.indices[triangle + 2];
        if (ia >= source.vertices.size() || ib >= source.vertices.size() ||
            ic >= source.vertices.size())
            continue;
        const Vertex& a = source.vertices[ia];
        const Vertex& b = source.vertices[ib];
        const Vertex& c = source.vertices[ic];
        const double denominator =
            (b.py - c.py) * (a.px - c.px) +
            (c.px - b.px) * (a.py - c.py);
        if (std::abs(denominator) < 1e-12) continue;

        const double triangle_min_x = std::min({a.px, b.px, c.px});
        const double triangle_max_x = std::max({a.px, b.px, c.px});
        const double triangle_min_y = std::min({a.py, b.py, c.py});
        const double triangle_max_y = std::max({a.py, b.py, c.py});
        const int first_x = std::clamp(
            static_cast<int>(std::floor((triangle_min_x - grid_min_x) / step_x)) - 1,
            0, columns - 1);
        const int last_x = std::clamp(
            static_cast<int>(std::ceil((triangle_max_x - grid_min_x) / step_x)) + 1,
            0, columns - 1);
        const int first_y = std::clamp(
            static_cast<int>(std::floor((triangle_min_y - grid_min_y) / step_y)) - 1,
            0, rows - 1);
        const int last_y = std::clamp(
            static_cast<int>(std::ceil((triangle_max_y - grid_min_y) / step_y)) + 1,
            0, rows - 1);
        bool hit = false;
        for (int y = first_y; y <= last_y; ++y) {
            const double py = grid_min_y + y * step_y;
            for (int x = first_x; x <= last_x; ++x) {
                const double px = grid_min_x + x * step_x;
                const double wa = ((b.py - c.py) * (px - c.px) +
                                   (c.px - b.px) * (py - c.py)) / denominator;
                const double wb = ((c.py - a.py) * (px - c.px) +
                                   (a.px - c.px) * (py - c.py)) / denominator;
                const double wc = 1.0 - wa - wb;
                constexpr double kInsideTolerance = -1e-6;
                if (wa < kInsideTolerance || wb < kInsideTolerance ||
                    wc < kInsideTolerance)
                    continue;
                const float z = static_cast<float>(wa * a.pz + wb * b.pz + wc * c.pz);
                const size_t index = sample_index(x, y);
                if (z <= heights[index]) continue;
                heights[index] = z;
                const float u = static_cast<float>(wa * a.u + wb * b.u + wc * c.u);
                const float v = static_cast<float>(wa * a.v + wb * b.v + wc * c.v);
                const auto colour = sample_rgb(source_atlas, source_atlas_size, u, v);
                for (int channel = 0; channel < 3; ++channel)
                    colours[index * 3 + channel] = colour[channel];
                hit = true;
            }
        }
        if (hit) ++rasterized_triangles;
    }

    // Close only one-cell sampling cracks. Requiring five neighbours avoids
    // flooding genuine courtyards or regions that have no source surface.
    for (int pass = 0; pass < 2; ++pass) {
        auto filled_heights = heights;
        auto filled_colours = colours;
        for (int y = 1; y + 1 < rows; ++y) {
            for (int x = 1; x + 1 < columns; ++x) {
                const size_t index = sample_index(x, y);
                if (std::isfinite(heights[index])) continue;
                double sum_height = 0.0;
                double sum_colour[3] = {0.0, 0.0, 0.0};
                int neighbours = 0;
                for (int oy = -1; oy <= 1; ++oy) {
                    for (int ox = -1; ox <= 1; ++ox) {
                        if (ox == 0 && oy == 0) continue;
                        const size_t neighbour = sample_index(x + ox, y + oy);
                        if (!std::isfinite(heights[neighbour])) continue;
                        sum_height += heights[neighbour];
                        for (int channel = 0; channel < 3; ++channel)
                            sum_colour[channel] += colours[neighbour * 3 + channel];
                        ++neighbours;
                    }
                }
                if (neighbours < 5) continue;
                filled_heights[index] = static_cast<float>(sum_height / neighbours);
                for (int channel = 0; channel < 3; ++channel)
                    filled_colours[index * 3 + channel] = static_cast<unsigned char>(
                        std::clamp(sum_colour[channel] / neighbours, 0.0, 255.0));
            }
        }
        heights.swap(filled_heights);
        colours.swap(filled_colours);
    }

    // Source OSGB roots are commonly clipped exactly at tile boundaries.  A
    // globally aligned grid can therefore leave its outermost sample just
    // outside the source triangle even though the neighbour reaches the same
    // lattice point.  Extend only the two-sample outer rim; unlike general
    // hole filling this does not flood courtyards or holes inside the tile.
    for (int pass = 0; pass < 2; ++pass) {
        auto filled_heights = heights;
        auto filled_colours = colours;
        for (int y = 0; y < rows; ++y) {
            for (int x = 0; x < columns; ++x) {
                if (x > 1 && x + 2 < columns && y > 1 && y + 2 < rows) continue;
                const size_t index = sample_index(x, y);
                if (std::isfinite(heights[index])) continue;
                double sum_height = 0.0;
                double sum_colour[3] = {0.0, 0.0, 0.0};
                int neighbours = 0;
                for (int oy = -1; oy <= 1; ++oy) {
                    for (int ox = -1; ox <= 1; ++ox) {
                        if (ox == 0 && oy == 0) continue;
                        const int sx = x + ox;
                        const int sy = y + oy;
                        if (sx < 0 || sx >= columns || sy < 0 || sy >= rows) continue;
                        const size_t neighbour = sample_index(sx, sy);
                        if (!std::isfinite(heights[neighbour])) continue;
                        sum_height += heights[neighbour];
                        for (int channel = 0; channel < 3; ++channel)
                            sum_colour[channel] += colours[neighbour * 3 + channel];
                        ++neighbours;
                    }
                }
                if (neighbours < 2) continue;
                filled_heights[index] = static_cast<float>(sum_height / neighbours);
                for (int channel = 0; channel < 3; ++channel)
                    filled_colours[index * 3 + channel] = static_cast<unsigned char>(
                        std::clamp(sum_colour[channel] / neighbours, 0.0, 255.0));
            }
        }
        heights.swap(filled_heights);
        colours.swap(filled_colours);
    }

    SurfaceProxy result;
    result.effective_spacing = std::max(step_x, step_y);
    std::vector<unsigned int> vertex_index(sample_count,
                                            std::numeric_limits<unsigned int>::max());
    auto height_at = [&](int x, int y, float fallback) {
        if (x < 0 || x >= columns || y < 0 || y >= rows) return fallback;
        const float value = heights[sample_index(x, y)];
        return std::isfinite(value) ? value : fallback;
    };
    for (int y = 0; y < rows; ++y) {
        for (int x = 0; x < columns; ++x) {
            const size_t index = sample_index(x, y);
            if (!std::isfinite(heights[index])) continue;
            Vertex vertex;
            vertex.px = static_cast<float>(grid_min_x + x * step_x);
            vertex.py = static_cast<float>(grid_min_y + y * step_y);
            vertex.pz = heights[index];
            const double dz_dx = (height_at(x + 1, y, vertex.pz) -
                                  height_at(x - 1, y, vertex.pz)) /
                                 ((x > 0 && x + 1 < columns) ? 2.0 * step_x : step_x);
            const double dz_dy = (height_at(x, y + 1, vertex.pz) -
                                  height_at(x, y - 1, vertex.pz)) /
                                 ((y > 0 && y + 1 < rows) ? 2.0 * step_y : step_y);
            const double length = std::sqrt(dz_dx * dz_dx + dz_dy * dz_dy + 1.0);
            vertex.nx = static_cast<float>(-dz_dx / length);
            vertex.ny = static_cast<float>(-dz_dy / length);
            vertex.nz = static_cast<float>(1.0 / length);
            vertex.u = static_cast<float>(x) / (columns - 1);
            vertex.v = static_cast<float>(y) / (rows - 1);
            vertex_index[index] = static_cast<unsigned int>(result.group.vertices.size());
            result.group.vertices.push_back(vertex);
        }
    }

    const unsigned int invalid = std::numeric_limits<unsigned int>::max();
    auto emit = [&](unsigned int a, unsigned int b, unsigned int c) {
        if (a == invalid || b == invalid || c == invalid) return;
        result.group.indices.push_back(a);
        result.group.indices.push_back(b);
        result.group.indices.push_back(c);
    };
    for (int y = 0; y + 1 < rows; ++y) {
        for (int x = 0; x + 1 < columns; ++x) {
            const unsigned int v00 = vertex_index[sample_index(x, y)];
            const unsigned int v10 = vertex_index[sample_index(x + 1, y)];
            const unsigned int v01 = vertex_index[sample_index(x, y + 1)];
            const unsigned int v11 = vertex_index[sample_index(x + 1, y + 1)];
            const int valid = static_cast<int>(v00 != invalid) +
                              static_cast<int>(v10 != invalid) +
                              static_cast<int>(v01 != invalid) +
                              static_cast<int>(v11 != invalid);
            if (valid == 4) {
                emit(v00, v10, v11);
                emit(v00, v11, v01);
            } else if (valid == 3) {
                if (v11 == invalid) emit(v00, v10, v01);
                else if (v01 == invalid) emit(v00, v10, v11);
                else if (v10 == invalid) emit(v00, v11, v01);
                else emit(v10, v11, v01);
            }
        }
    }
    if (result.group.indices.empty())
        throw std::runtime_error("surface reconstruction produced no triangles");

    result.texture_size = std::max(columns, rows);
    result.texture.assign(
        static_cast<size_t>(result.texture_size) * result.texture_size * 3, 96);
    for (int ty = 0; ty < result.texture_size; ++ty) {
        const int gy = static_cast<int>(std::lround(
            static_cast<double>(ty) * (rows - 1) / (result.texture_size - 1)));
        for (int tx = 0; tx < result.texture_size; ++tx) {
            const int gx = static_cast<int>(std::lround(
                static_cast<double>(tx) * (columns - 1) / (result.texture_size - 1)));
            size_t source_index = sample_index(gx, gy);
            if (!std::isfinite(heights[source_index])) {
                bool found = false;
                for (int radius = 1; radius <= 8 && !found; ++radius) {
                    for (int oy = -radius; oy <= radius && !found; ++oy) {
                        for (int ox = -radius; ox <= radius; ++ox) {
                            const int sx = gx + ox;
                            const int sy = gy + oy;
                            if (sx < 0 || sx >= columns || sy < 0 || sy >= rows) continue;
                            const size_t candidate = sample_index(sx, sy);
                            if (!std::isfinite(heights[candidate])) continue;
                            source_index = candidate;
                            found = true;
                            break;
                        }
                    }
                }
            }
            const size_t destination =
                (static_cast<size_t>(ty) * result.texture_size + tx) * 3;
            for (int channel = 0; channel < 3; ++channel)
                result.texture[destination + channel] = colours[source_index * 3 + channel];
        }
    }
    std::cout << "  HLOD surface: source_triangles="
              << source.indices.size() / 3
              << " rasterized=" << rasterized_triangles
              << " grid=" << columns << "x" << rows
              << " spacing=" << result.effective_spacing
              << " output_triangles=" << result.group.indices.size() / 3 << "\n";
    return result;
}

static void simplify_group(Group& group, double ratio, double max_error,
                           bool lock_border = true) {
    if (group.indices.size() < 6 || group.vertices.empty() || ratio >= 0.999) return;
    size_t old_indices = group.indices.size();
    size_t target = static_cast<size_t>(std::floor(old_indices * ratio / 3.0)) * 3;
    target = std::max<size_t>(3, target);
    std::vector<unsigned int> simplified(old_indices);
    float result_error = 0.0f;
    // Attribute-aware topological simplification preserves material/UV seams.
    // ErrorAbsolute makes the quality bound independent of the physical extent
    // of the GLB. Border locking is useful for a single connected source mesh,
    // but must be disabled for merged HLODs: every disconnected building part
    // otherwise looks like a border and virtually no simplification occurs.
    const float attribute_weights[5] = {0.10f, 0.10f, 0.10f, 0.05f, 0.05f};
    unsigned int options = meshopt_SimplifyErrorAbsolute |
                           meshopt_SimplifyPermissive;
    if (lock_border) options |= meshopt_SimplifyLockBorder;
    size_t count = meshopt_simplifyWithAttributes(
        simplified.data(), group.indices.data(), group.indices.size(),
        &group.vertices[0].px, group.vertices.size(), sizeof(Vertex),
        &group.vertices[0].nx, sizeof(Vertex), attribute_weights, 5, nullptr,
        target, static_cast<float>(max_error), options,
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

static void simplify_hlod_components(Group& group, double ratio,
                                     double max_error,
                                     size_t max_triangles) {
    if (group.indices.empty() || group.vertices.empty()) return;

    const size_t max_indices = max_triangles > 0
        ? max_triangles * 3
        : group.indices.size();
    const double budget_ratio = group.indices.size() > max_indices
        ? static_cast<double>(max_indices) / group.indices.size()
        : 1.0;
    const double effective_ratio = std::min(ratio, budget_ratio);

    if (group.components.empty()) {
        simplify_group(group, effective_ratio, max_error, false);
        return;
    }

    Group simplified_group;
    simplified_group.vertices.reserve(std::min(
        group.vertices.size(), max_indices));
    simplified_group.indices.reserve(std::min(
        group.indices.size(), max_indices));

    for (const ComponentRange& range : group.components) {
        if (range.vertex_count == 0 || range.index_count < 3) continue;
        if (range.vertex_offset + range.vertex_count > group.vertices.size()
            || range.index_offset + range.index_count > group.indices.size())
            throw std::runtime_error("invalid source primitive range");

        Group component;
        component.vertices.assign(
            group.vertices.begin() + range.vertex_offset,
            group.vertices.begin() + range.vertex_offset + range.vertex_count);
        component.indices.reserve(range.index_count);
        for (size_t i = 0; i < range.index_count; ++i) {
            const unsigned int source_index =
                group.indices[range.index_offset + i];
            if (source_index < range.vertex_offset
                || source_index >= range.vertex_offset + range.vertex_count)
                throw std::runtime_error("source primitive index is out of range");
            component.indices.push_back(static_cast<unsigned int>(
                source_index - range.vertex_offset));
        }

        // Simplify each source primitive independently. This guarantees that a
        // large terrain/building mesh cannot consume the budget of every small
        // disconnected component. Absolute error stops before visible damage
        // even when the requested triangle target cannot safely be reached.
        simplify_group(component, effective_ratio, max_error, false);
        if (component.indices.empty() || component.vertices.empty()) continue;

        const unsigned int output_base = static_cast<unsigned int>(
            simplified_group.vertices.size());
        simplified_group.vertices.insert(simplified_group.vertices.end(),
                                         component.vertices.begin(),
                                         component.vertices.end());
        for (unsigned int index : component.indices)
            simplified_group.indices.push_back(output_base + index);
    }

    group.vertices.swap(simplified_group.vertices);
    group.indices.swap(simplified_group.indices);
    group.components.clear();
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

    // Draco quantization is relative to the complete primitive bounds.  A
    // fixed 11-bit grid is adequate for a small source tile but can have a
    // multi-metre step for a root HLOD. Raise the precision as the merged
    // extent grows, targeting at most 25 cm per position step (up to 20 bits).
    float minimum[3] = {group.vertices[0].px, group.vertices[0].py,
                        group.vertices[0].pz};
    float maximum[3] = {minimum[0], minimum[1], minimum[2]};
    for (const Vertex& vertex : group.vertices) {
        const float p[3] = {vertex.px, vertex.py, vertex.pz};
        for (int component = 0; component < 3; ++component) {
            minimum[component] = std::min(minimum[component], p[component]);
            maximum[component] = std::max(maximum[component], p[component]);
        }
    }
    const double max_extent = std::max({
        static_cast<double>(maximum[0] - minimum[0]),
        static_cast<double>(maximum[1] - minimum[1]),
        static_cast<double>(maximum[2] - minimum[2])});
    const int extent_bits = max_extent > 0.0
        ? static_cast<int>(std::ceil(std::log2(max_extent / 0.25 + 1.0)))
        : 1;
    const int effective_position_bits = std::clamp(
        std::max(position_bits, extent_bits), 1, 20);

    draco::Encoder encoder;
    encoder.SetSpeedOptions(5, 5);
    encoder.SetAttributeQuantization(draco::GeometryAttribute::POSITION,
                                     effective_position_bits);
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
                              double surface_error,
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

        auto source_atlases = build_atlas_pixels(input, atlas_count, atlas_size, grid);
        auto source_groups = merge_geometry(input, atlas_count, atlas_size, grid);
        if (source_groups.size() != 1 || source_atlases.size() != 1)
            throw std::runtime_error("HLOD surface reconstruction requires one source atlas");
        SurfaceProxy surface = reconstruct_top_surface(
            source_groups.front(), source_atlases.front(), atlas_size,
            atlas_cell_size, surface_error);
        std::vector<Group> groups;
        groups.push_back(std::move(surface.group));
        std::vector<std::vector<unsigned char>> atlases;
        atlases.push_back(std::move(surface.texture));
        tinygltf::Model output = build_output(
            groups, atlases, surface.texture_size,
            enable_texture_compress, ktx2_quality,
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
                    input_bytes, output_bytes, options.atlas_size,
                    options.max_error,
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
