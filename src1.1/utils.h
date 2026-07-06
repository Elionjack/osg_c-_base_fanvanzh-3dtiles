#pragma once

#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <fstream>
#include <sys/stat.h>
#include <cmath>
#include <algorithm>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#endif

// ============================================================
// Logging macros (printf-based, no spdlog dependency)
// ============================================================
inline void log_printf_impl(const char* level, const char* format, ...) {
    char buf[2048];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    fprintf(stderr, "[%s] %s\n", level, buf);
}

#define LOG_D(format, ...) log_printf_impl("DEBUG", format, ##__VA_ARGS__)
#define LOG_I(format, ...) log_printf_impl("INFO",  format, ##__VA_ARGS__)
#define LOG_W(format, ...) log_printf_impl("WARN",  format, ##__VA_ARGS__)
#define LOG_E(format, ...) log_printf_impl("ERROR", format, ##__VA_ARGS__)

// ============================================================
// File I/O (replaces Rust FFI write_file / mkdirs)
// ============================================================
inline bool write_file(const char* filename, const char* buf, unsigned long buf_len) {
    std::string path(filename);
    // Create parent directories
    auto pos = path.find_last_of("/\\");
    if (pos != std::string::npos) {
        std::string parent = path.substr(0, pos);
#ifdef _WIN32
        // Recursive mkdir on Windows
        std::string cmd = "mkdir \"" + parent + "\" 2>nul";
        // Use CreateDirectory approach instead
        for (size_t i = 0; i < parent.size(); i++) {
            if (parent[i] == '/' || parent[i] == '\\') {
                if (i > 0) {
                    std::string sub = parent.substr(0, i);
#ifdef _WIN32
                    _mkdir(sub.c_str());
#else
                    mkdir(sub.c_str(), 0755);
#endif
                }
            }
        }
        // Create final directory
#ifdef _WIN32
        _mkdir(parent.c_str());
#else
        mkdir(parent.c_str(), 0755);
#endif
#endif
    }
    std::ofstream ofs(filename, std::ios::binary);
    if (!ofs.is_open()) {
        LOG_E("write_file: cannot open %s", filename);
        return false;
    }
    ofs.write(buf, buf_len);
    ofs.close();
    return true;
}

inline bool mkdirs(const char* path) {
#ifdef _WIN32
    std::string p(path);
    for (size_t i = 0; i < p.size(); i++) {
        if (p[i] == '/' || p[i] == '\\') {
            if (i > 0) _mkdir(p.substr(0, i).c_str());
        }
    }
    return _mkdir(p.c_str()) == 0 || errno == EEXIST;
#else
    std::string p(path);
    for (size_t i = 0; i < p.size(); i++) {
        if (p[i] == '/') {
            if (i > 0) mkdir(p.substr(0, i).c_str(), 0755);
        }
    }
    return mkdir(p.c_str(), 0755) == 0 || errno == EEXIST;
#endif
}

// ============================================================
// Binary buffer helpers
// ============================================================
template<class T>
inline void put_val(std::vector<unsigned char>& buf, T val) {
    buf.insert(buf.end(), (unsigned char*)&val, (unsigned char*)&val + sizeof(T));
}

template<class T>
inline void put_val(std::string& buf, T val) {
    buf.append((const char*)&val, sizeof(T));
}

template<class T>
inline void alignment_buffer(std::vector<T>& buf) {
    while (buf.size() % 8 != 0) {
        buf.push_back(0x00);
    }
}

template<class T>
inline void alignment_buffer_4(std::vector<T>& buf) {
    while (buf.size() % 4 != 0) {
        buf.push_back(0x00);
    }
}

// ============================================================
// Path utilities
// ============================================================
inline std::string get_file_name(std::string path) {
    auto p0 = path.find_last_of("/\\");
    if (p0 == std::string::npos) return path;
    return path.substr(p0 + 1);
}

inline std::string get_parent(std::string str) {
    auto p0 = str.find_last_of("/\\");
    if (p0 != std::string::npos)
        return str.substr(0, p0);
    else
        return "";
}

inline std::string replace(std::string str, std::string s0, std::string s1) {
    auto p0 = str.find(s0);
    if (p0 == std::string::npos) return str;
    return str.replace(p0, s0.length(), s1);
}

inline int get_lvl_num(std::string file_name) {
    std::string stem = get_file_name(file_name);
    auto p0 = stem.find("_L");
    auto p1 = stem.find("_", p0 + 2);
    if (p0 != std::string::npos && p1 != std::string::npos) {
        std::string substr = stem.substr(p0 + 2, p1 - p0 - 2);
        try { return std::stol(substr); }
        catch (...) { return -1; }
    } else if (p0 != std::string::npos) {
        int end = p0 + 2;
        while (true) {
            if (isdigit(stem[end])) end++;
            else break;
        }
        std::string substr = stem.substr(p0 + 2, end - p0 - 2);
        try { return std::stol(substr); }
        catch (...) { return -1; }
    }
    return -1;
}

// ============================================================
// Math / Bounding box helpers
// ============================================================
static const double PI = std::acos(-1.0);

inline double degree2rad(double val) { return val * PI / 180.0; }

inline double lati_to_meter(double diff) { return diff / 0.000000157891; }
inline double longti_to_meter(double diff, double lati) { return diff / 0.000000156785 * std::cos(lati); }
inline double meter_to_lati(double m) { return m * 0.000000157891; }
inline double meter_to_longti(double m, double lati) { return m * 0.000000156785 / std::cos(lati); }

// ============================================================
// Core data structures
// ============================================================
struct Transform {
    double radian_x;
    double radian_y;
    double min_height;
};

struct Box {
    double matrix[12];
};

struct Region {
    double min_x, min_y, max_x, max_y, min_height, max_height;
};

struct TileBox {
    std::vector<double> max;
    std::vector<double> min;

    void extend(double ratio) {
        ratio /= 2;
        if (max.empty() || min.empty()) return;
        double x = max[0] - min[0];
        double y = max[1] - min[1];
        double z = max[2] - min[2];
        max[0] += x * ratio;
        max[1] += y * ratio;
        max[2] += z * ratio;
        min[0] -= x * ratio;
        min[1] -= y * ratio;
        min[2] -= z * ratio;
    }
};

inline double get_geometric_error(TileBox& bbox) {
    if (bbox.max.empty() || bbox.min.empty()) return 0;
    double max_err = std::max((bbox.max[0] - bbox.min[0]), (bbox.max[1] - bbox.min[1]));
    max_err = std::max(max_err, (bbox.max[2] - bbox.min[2]));
    return max_err / 2.0;
}

inline std::vector<double> convert_bbox(TileBox tile) {
    double center_mx = (tile.max[0] + tile.min[0]) / 2;
    double center_my = (tile.max[1] + tile.min[1]) / 2;
    double center_mz = (tile.max[2] + tile.min[2]) / 2;
    double x_meter = (tile.max[0] - tile.min[0]) * 1;
    double y_meter = (tile.max[1] - tile.min[1]) * 1;
    double z_meter = (tile.max[2] - tile.min[2]) * 1;
    if (x_meter < 0.01) x_meter = 0.01;
    if (y_meter < 0.01) y_meter = 0.01;
    if (z_meter < 0.01) z_meter = 0.01;
    return {
        center_mx, center_my, center_mz,
        x_meter / 2, 0.0, 0.0,
        0.0, y_meter / 2, 0.0,
        0.0, 0.0, z_meter / 2
    };
}
