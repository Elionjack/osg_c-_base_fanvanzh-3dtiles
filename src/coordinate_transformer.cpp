#include "coordinate_transformer.h"
#include <cstdio>
#include <cmath>

namespace coords {

// WGS84 ellipsoid parameters
static constexpr double WGS84_A  = 6378137.0;
static constexpr double WGS84_F  = 1.0 / 298.257223563;
static constexpr double WGS84_E2 = WGS84_F * (2.0 - WGS84_F);

// ============================================================
// Constructor / Destructor / Move
// ============================================================
CoordinateTransformer::CoordinateTransformer(const CoordinateSystem& cs)
    : source_cs_(cs), mode_(TransformMode::None) {}

CoordinateTransformer::CoordinateTransformer(const CoordinateSystem& cs,
                                             const GeoReference& geo_ref)
    : source_cs_(cs), mode_(TransformMode::WithGeoReference), geoid_config_(GeoidConfig::Disabled()) {
    InitializeWithGeoRef(geo_ref);
}

CoordinateTransformer::CoordinateTransformer(const CoordinateSystem& cs,
                                             const GeoReference& geo_ref,
                                             const GeoidConfig& geoid_config)
    : source_cs_(cs), mode_(TransformMode::WithGeoReference), geoid_config_(geoid_config) {
    InitializeWithGeoRef(geo_ref);
}

CoordinateTransformer::~CoordinateTransformer() = default;

CoordinateTransformer::CoordinateTransformer(CoordinateTransformer&& other) noexcept
    : source_cs_(std::move(other.source_cs_))
    , mode_(other.mode_)
    , geo_origin_lon_(other.geo_origin_lon_)
    , geo_origin_lat_(other.geo_origin_lat_)
    , geo_origin_height_(other.geo_origin_height_)
    , enu_to_ecef_(other.enu_to_ecef_)
    , ecef_to_enu_(other.ecef_to_enu_)
    , axis_transform_(other.axis_transform_)
    , ogr_transform_(std::move(other.ogr_transform_))
    , geoid_config_(other.geoid_config_) {}

CoordinateTransformer& CoordinateTransformer::operator=(CoordinateTransformer&& other) noexcept {
    if (this != &other) {
        source_cs_ = std::move(other.source_cs_);
        mode_ = other.mode_;
        geo_origin_lon_ = other.geo_origin_lon_;
        geo_origin_lat_ = other.geo_origin_lat_;
        geo_origin_height_ = other.geo_origin_height_;
        enu_to_ecef_ = other.enu_to_ecef_;
        ecef_to_enu_ = other.ecef_to_enu_;
        axis_transform_ = other.axis_transform_;
        ogr_transform_ = std::move(other.ogr_transform_);
        geoid_config_ = other.geoid_config_;
    }
    return *this;
}

// ============================================================
// Initialization
// ============================================================
void CoordinateTransformer::InitializeWithGeoRef(const GeoReference& geo_ref) {
    if (source_cs_.Type() == CoordinateType::ENU) {
        auto enu_params = source_cs_.GetENUParams();
        if (enu_params) {
            geo_origin_lon_ = enu_params->origin_lon;
            geo_origin_lat_ = enu_params->origin_lat;
            geo_origin_height_ = enu_params->origin_height;
        }
    } else if (source_cs_.NeedsOGRTransform()) {
        CreateOGRTransform();
        if (geo_ref.lon != 0.0 || geo_ref.lat != 0.0) {
            geo_origin_lon_ = geo_ref.lon;
            geo_origin_lat_ = geo_ref.lat;
            geo_origin_height_ = geo_ref.height;
            if (geoid_config_.enabled && GeoidHeight::GetGlobalGeoidCalculator().IsInitialized()) {
                geo_origin_height_ = ApplyGeoidCorrection(geo_origin_lat_, geo_origin_lon_, geo_origin_height_);
            }
        } else {
            auto [origin_x, origin_y, origin_z] = source_cs_.GetSourceOrigin();
            glm::dvec3 origin{origin_x, origin_y, origin_z};
            if (ogr_transform_) {
                ogr_transform_->Transform(1, &origin.x, &origin.y, &origin.z);
            }
            geo_origin_lon_ = origin.x;
            geo_origin_lat_ = origin.y;
            geo_origin_height_ = origin.z;
            geo_origin_height_ = ApplyGeoidCorrection(geo_origin_lat_, geo_origin_lon_, geo_origin_height_);
        }
        fprintf(stderr, "[CoordinateTransformer] OGR result: lon=%.10f lat=%.10f h=%.3f\n",
                geo_origin_lon_, geo_origin_lat_, geo_origin_height_);
    } else {
        geo_origin_lon_ = geo_ref.lon;
        geo_origin_lat_ = geo_ref.lat;
        geo_origin_height_ = geo_ref.height;
    }

    enu_to_ecef_ = CalcEnuToEcefMatrix(geo_origin_lon_, geo_origin_lat_, geo_origin_height_);
    ecef_to_enu_ = glm::inverse(enu_to_ecef_);
    axis_transform_ = GetAxisTransformMatrix(source_cs_.GetUpAxis(), UpAxis::Y_UP);

    fprintf(stderr, "[CoordinateTransformer] Initialized: origin=(%.10f, %.10f, %.3f)\n",
            geo_origin_lon_, geo_origin_lat_, geo_origin_height_);
}

void CoordinateTransformer::CreateOGRTransform() {
    OGRSpatialReference outRs;
    outRs.importFromEPSG(4326);
    outRs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

    OGRSpatialReference inRs;
    inRs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

    if (source_cs_.Type() == CoordinateType::EPSG) {
        auto code = source_cs_.GetEPSGCode();
        if (code) inRs.importFromEPSG(*code);
    } else if (source_cs_.Type() == CoordinateType::WKT) {
        auto wkt = source_cs_.GetWKTString();
        if (wkt) inRs.importFromWkt(wkt->c_str());
    }

    OGRCoordinateTransformation* poCT = OGRCreateCoordinateTransformation(&inRs, &outRs);
    if (poCT) {
        ogr_transform_.reset(poCT);
        fprintf(stderr, "[CoordinateTransformer] OGR transform created\n");
    } else {
        fprintf(stderr, "[CoordinateTransformer] Failed to create OGR transform\n");
    }
}

// ============================================================
// Geoid correction
// ============================================================
bool CoordinateTransformer::ShouldApplyGeoidCorrection() const {
    if (!geoid_config_.enabled) return false;
    if (!GeoidHeight::GetGlobalGeoidCalculator().IsInitialized()) return false;
    switch (source_cs_.Type()) {
        case CoordinateType::EPSG:
        case CoordinateType::WKT: {
            auto datum = source_cs_.GetVerticalDatum();
            return datum == VerticalDatum::Orthometric || datum == VerticalDatum::Unknown;
        }
        case CoordinateType::ENU:
        case CoordinateType::LocalCartesian:
            return false;
        default: return false;
    }
}

double CoordinateTransformer::ApplyGeoidCorrection(double lat, double lon, double height) const {
    if (!ShouldApplyGeoidCorrection()) return height;
    double corrected = GeoidHeight::GetGlobalGeoidCalculator()
        .ConvertOrthometricToEllipsoidal(lat, lon, height);
    fprintf(stderr, "[CoordinateTransformer] Geoid: ortho=%.3f -> ellip=%.3f\n", height, corrected);
    return corrected;
}

// ============================================================
// Coordinate transforms
// ============================================================
glm::dvec3 CoordinateTransformer::ToWGS84(const glm::dvec3& point) const {
    if (!HasGeoReference()) {
        fprintf(stderr, "[CoordinateTransformer] Warning: ToWGS84 called without geo ref\n");
        return point;
    }
    glm::dvec3 result = point;
    result = axis_transform_ * glm::dvec4(result, 1.0);

    if (source_cs_.Type() == CoordinateType::ENU) {
        auto enu_params = source_cs_.GetENUParams();
        if (enu_params) {
            result.x += enu_params->offset_x;
            result.y += enu_params->offset_y;
            result.z += enu_params->offset_z;
        }
        glm::dvec3 ecef = enu_to_ecef_ * glm::dvec4(result, 1.0);
        return {geo_origin_lon_, geo_origin_lat_, geo_origin_height_ + result.z};
    } else if (source_cs_.NeedsOGRTransform() && ogr_transform_) {
        auto [origin_x, origin_y, origin_z] = source_cs_.GetSourceOrigin();
        result.x += origin_x;
        result.y += origin_y;
        result.z += origin_z;
        ogr_transform_->Transform(1, &result.x, &result.y, &result.z);
    } else {
        result = {geo_origin_lon_, geo_origin_lat_, geo_origin_height_ + result.z};
    }
    return result;
}

glm::dvec3 CoordinateTransformer::ToECEF(const glm::dvec3& point) const {
    if (!HasGeoReference()) {
        fprintf(stderr, "[CoordinateTransformer] Warning: ToECEF called without geo ref\n");
        return point;
    }
    glm::dvec3 result = point;
    result = axis_transform_ * glm::dvec4(result, 1.0);

    if (source_cs_.Type() == CoordinateType::ENU) {
        auto enu_params = source_cs_.GetENUParams();
        if (enu_params) {
            result.x += enu_params->offset_x;
            result.y += enu_params->offset_y;
            result.z += enu_params->offset_z;
        }
        return enu_to_ecef_ * glm::dvec4(result, 1.0);
    } else {
        glm::dvec3 wgs84 = ToWGS84(point);
        return CartographicToEcef(wgs84.x, wgs84.y, wgs84.z);
    }
}

glm::dvec3 CoordinateTransformer::ToLocalENU(const glm::dvec3& point) const {
    if (!HasGeoReference()) {
        fprintf(stderr, "[CoordinateTransformer] Warning: ToLocalENU called without geo ref\n");
        return point;
    }
    glm::dvec3 result = point;

    if (source_cs_.Type() == CoordinateType::ENU) {
        auto enu_params = source_cs_.GetENUParams();
        if (enu_params) {
            result.x += enu_params->offset_x;
            result.y += enu_params->offset_y;
            result.z += enu_params->offset_z;
        }
        glm::dvec3 ecef = enu_to_ecef_ * glm::dvec4(result, 1.0);
        glm::dvec4 enu = ecef_to_enu_ * glm::dvec4(ecef, 1.0);
        return {enu.x, enu.y, enu.z};
    } else if (source_cs_.NeedsOGRTransform() && ogr_transform_) {
        auto [origin_x, origin_y, origin_z] = source_cs_.GetSourceOrigin();
        result.x += origin_x;
        result.y += origin_y;
        result.z += origin_z;
        ogr_transform_->Transform(1, &result.x, &result.y, &result.z);
        result.z = ApplyGeoidCorrection(result.y, result.x, result.z);
        glm::dvec3 ecef = CartographicToEcef(result.x, result.y, result.z);
        glm::dvec4 enu = ecef_to_enu_ * glm::dvec4(ecef, 1.0);
        return {enu.x, enu.y, enu.z};
    } else {
        return result;
    }
}

void CoordinateTransformer::TransformToWGS84(std::vector<glm::dvec3>& points) const {
    for (auto& p : points) p = ToWGS84(p);
}

void CoordinateTransformer::TransformToLocalENU(std::vector<glm::dvec3>& points) const {
    for (auto& p : points) p = ToLocalENU(p);
}

glm::dvec3 CoordinateTransformer::ConvertUpAxis(const glm::dvec3& point, UpAxis target_axis) const {
    glm::dmat4 transform = GetAxisTransformMatrix(source_cs_.GetUpAxis(), target_axis);
    glm::dvec4 result = transform * glm::dvec4(point, 1.0);
    return {result.x, result.y, result.z};
}

// ============================================================
// Static methods
// ============================================================
glm::dmat4 CoordinateTransformer::CalcEnuToEcefMatrix(double lon_deg, double lat_deg, double height) {
    double lon = lon_deg * M_PI / 180.0;
    double phi = lat_deg * M_PI / 180.0;

    double sinPhi = std::sin(phi), cosPhi = std::cos(phi);
    double sinLon = std::sin(lon), cosLon = std::cos(lon);
    double N = WGS84_A / std::sqrt(1.0 - WGS84_E2 * sinPhi * sinPhi);

    double x0 = (N + height) * cosPhi * cosLon;
    double y0 = (N + height) * cosPhi * sinLon;
    double z0 = (N * (1.0 - WGS84_E2) + height) * sinPhi;

    glm::dvec3 east (-sinLon,           cosLon,           0.0);
    glm::dvec3 north(-sinPhi * cosLon,  -sinPhi * sinLon, cosPhi);
    glm::dvec3 up   ( cosPhi * cosLon,   cosPhi * sinLon, sinPhi);

    glm::dmat4 T(1.0);
    T[0] = glm::dvec4(east,  0.0);
    T[1] = glm::dvec4(north, 0.0);
    T[2] = glm::dvec4(up,    0.0);
    T[3] = glm::dvec4(x0, y0, z0, 1.0);
    return T;
}

glm::dvec3 CoordinateTransformer::CartographicToEcef(double lon_deg, double lat_deg, double height) {
    double lon = lon_deg * M_PI / 180.0;
    double phi = lat_deg * M_PI / 180.0;

    double sinPhi = std::sin(phi), cosPhi = std::cos(phi);
    double sinLon = std::sin(lon), cosLon = std::cos(lon);
    double N = WGS84_A / std::sqrt(1.0 - WGS84_E2 * sinPhi * sinPhi);

    double x = (N + height) * cosPhi * cosLon;
    double y = (N + height) * cosPhi * sinLon;
    double z = (N * (1.0 - WGS84_E2) + height) * sinPhi;
    return {x, y, z};
}

glm::dmat4 CoordinateTransformer::GetAxisTransformMatrix(UpAxis from, UpAxis to) {
    if (from == to) return glm::dmat4(1.0);

    if (from == UpAxis::Z_UP && to == UpAxis::Y_UP) {
        // Z-Up → Y-Up: (x, y, z) → (x, -z, y)
        return glm::dmat4(
            1,  0,  0, 0,
            0,  0,  1, 0,
            0, -1,  0, 0,
            0,  0,  0, 1
        );
    } else {
        // Y-Up → Z-Up: (x, y, z) → (x, z, -y)
        return glm::dmat4(
            1,  0,  0, 0,
            0,  0, -1, 0,
            0,  1,  0, 0,
            0,  0,  0, 1
        );
    }
}

} // namespace coords
