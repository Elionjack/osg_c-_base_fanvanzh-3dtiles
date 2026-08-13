#pragma once

#include <string>

// Applies the same one-atlas/one-primitive HLOD optimization used by the
// standalone root_glb_optimizer, but operates entirely in memory so src1.1
// can use it before writing each generated HLOD GLB.
bool optimize_hlod_glb_buffer(const std::string& input_glb,
                              std::string& output_glb,
                              int atlas_size,
                              double ratio,
                              double max_error,
                              int jpeg_quality,
                              std::string* error_message = nullptr);
