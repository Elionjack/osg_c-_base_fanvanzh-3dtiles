#pragma once

#include <string>

// Applies the one-atlas/one-primitive HLOD optimization in memory before
// src1.1 writes each generated HLOD GLB.
bool optimize_hlod_glb_buffer(const std::string& input_glb,
                              std::string& output_glb,
                              int atlas_cell_size,
                              double surface_error,
                              bool enable_texture_compress,
                              int ktx2_quality,
                              int draco_position_bits,
                              int draco_normal_bits,
                              int draco_uv_bits,
                              std::string* error_message = nullptr);
