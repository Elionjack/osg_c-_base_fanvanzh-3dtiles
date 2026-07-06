#pragma once

#include <string>
#include <variant>
#include <optional>
#include <tuple>

// Forward declaration for glm (include in cpp)
#ifndef GLM_FORCE_CTOR_INIT
#define GLM_FORCE_CTOR_INIT
#endif
#include <glm/glm.hpp>

namespace coords {

// ============================================================
// Enums
// ============================================================
enum class CoordinateType {
    Unknown,
    LocalCartesian,
    ENU,
    EPSG,
    WKT
};

enum class UpAxis {
    Y_UP,   // Y axis up (FBX, glTF)
    Z_UP    // Z axis up (OSGB)
};

enum class Handedness {
    RightHanded,
    LeftHanded
};

enum class VerticalDatum {
    Ellipsoidal,    // Ellipsoidal height (no correction needed)
    Orthometric,    // Orthometric height (needs Geoid correction)
    Unknown
};

// ============================================================
// Parameter structs
// ============================================================
struct GeoReference {
    double lon = 0.0;
    double lat = 0.0;
    double height = 0.0;
    VerticalDatum datum = VerticalDatum::Ellipsoidal;

    static GeoReference FromDegrees(double lon, double lat, double height,
                                    VerticalDatum datum = VerticalDatum::Ellipsoidal) {
        return {lon, lat, height, datum};
    }
};

struct LocalCartesianParams {
    UpAxis up_axis = UpAxis::Y_UP;
    Handedness handedness = Handedness::RightHanded;

    static LocalCartesianParams YUp() { return {UpAxis::Y_UP, Handedness::RightHanded}; }
    static LocalCartesianParams ZUp() { return {UpAxis::Z_UP, Handedness::RightHanded}; }
};

struct ENUParams {
    double origin_lon = 0.0;
    double origin_lat = 0.0;
    double origin_height = 0.0;
    double offset_x = 0.0;
    double offset_y = 0.0;
    double offset_z = 0.0;

    GeoReference GetGeoReference() const {
        return {origin_lon, origin_lat, origin_height, VerticalDatum::Ellipsoidal};
    }
};

struct EPSGParams {
    int code = 0;
    double origin_x = 0.0;
    double origin_y = 0.0;
    double origin_z = 0.0;
    VerticalDatum vertical_datum = VerticalDatum::Unknown;
};

struct WKTParams {
    std::string wkt;
    double origin_x = 0.0;
    double origin_y = 0.0;
    double origin_z = 0.0;
    VerticalDatum vertical_datum = VerticalDatum::Unknown;
};

// ============================================================
// CoordinateSystem class
// ============================================================
class CoordinateSystem {
public:
    CoordinateSystem() = default;

    // Factory methods
    static CoordinateSystem LocalCartesian(UpAxis up_axis = UpAxis::Y_UP,
                                           Handedness handedness = Handedness::RightHanded);
    static CoordinateSystem LocalCartesian(const LocalCartesianParams& params);

    static CoordinateSystem ENU(double lon, double lat, double height,
                                double offset_x, double offset_y, double offset_z);

    static CoordinateSystem EPSG(int code,
                                 double origin_x, double origin_y, double origin_z,
                                 VerticalDatum vertical_datum = VerticalDatum::Unknown);

    static CoordinateSystem WKT(const std::string& wkt,
                                double origin_x, double origin_y, double origin_z,
                                VerticalDatum vertical_datum = VerticalDatum::Unknown);

    // Query methods
    CoordinateType Type() const { return type_; }
    bool IsValid() const { return type_ != CoordinateType::Unknown; }
    bool NeedsOGRTransform() const;
    bool HasBuiltinGeoReference() const;

    std::optional<GeoReference> GetBuiltinGeoReference() const;
    std::tuple<double, double, double> GetSourceOrigin() const;

    std::optional<ENUParams> GetENUParams() const;
    std::optional<LocalCartesianParams> GetLocalCartesianParams() const;
    std::optional<int> GetEPSGCode() const;
    std::optional<std::string> GetWKTString() const;

    VerticalDatum GetVerticalDatum() const;
    void SetVerticalDatum(VerticalDatum datum);

    UpAxis GetUpAxis() const;
    Handedness GetHandedness() const;

    std::string ToString() const;

private:
    CoordinateType type_ = CoordinateType::Unknown;
    std::variant<std::monostate, LocalCartesianParams, ENUParams, EPSGParams, WKTParams> params_;
};

} // namespace coords
