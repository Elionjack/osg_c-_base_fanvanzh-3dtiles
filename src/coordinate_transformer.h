#pragma once

#include "coordinate_system.h"
#include "geoid_height.h"
#include <ogr_spatialref.h>
#include <memory>
#include <vector>
#include <glm/glm.hpp>

namespace coords {

// ============================================================
// GeoidConfig
// ============================================================
struct GeoidConfig {
    bool enabled = false;
    GeoidHeight::GeoidModel model = GeoidHeight::GeoidModel::EGM96;
    std::string data_path;

    static GeoidConfig Disabled() { return {false}; }
    static GeoidConfig EGM96(const std::string& path = "") {
        return {true, GeoidHeight::GeoidModel::EGM96, path};
    }
    static GeoidConfig EGM2008(const std::string& path = "") {
        return {true, GeoidHeight::GeoidModel::EGM2008, path};
    }
};

enum class TransformMode {
    None,
    WithGeoReference
};

// ============================================================
// CoordinateTransformer
// ============================================================
class CoordinateTransformer {
public:
    // Mode 1: No geographic reference (pure format conversion)
    explicit CoordinateTransformer(const CoordinateSystem& cs);

    // Mode 2: With geographic reference
    CoordinateTransformer(const CoordinateSystem& cs, const GeoReference& geo_ref);

    // Mode 3: With geographic reference and Geoid config
    CoordinateTransformer(const CoordinateSystem& cs, const GeoReference& geo_ref,
                          const GeoidConfig& geoid_config);

    ~CoordinateTransformer();

    // No copy, move only
    CoordinateTransformer(const CoordinateTransformer&) = delete;
    CoordinateTransformer& operator=(const CoordinateTransformer&) = delete;
    CoordinateTransformer(CoordinateTransformer&&) noexcept;
    CoordinateTransformer& operator=(CoordinateTransformer&&) noexcept;

    // Mode queries
    TransformMode GetMode() const { return mode_; }
    bool HasGeoReference() const { return mode_ == TransformMode::WithGeoReference; }

    // Coordinate transforms (only valid with HasGeoReference)
    glm::dvec3 ToWGS84(const glm::dvec3& point) const;
    glm::dvec3 ToECEF(const glm::dvec3& point) const;
    glm::dvec3 ToLocalENU(const glm::dvec3& point) const;

    void TransformToWGS84(std::vector<glm::dvec3>& points) const;
    void TransformToLocalENU(std::vector<glm::dvec3>& points) const;

    // Axis conversion (always available)
    glm::dvec3 ConvertUpAxis(const glm::dvec3& point, UpAxis target_axis = UpAxis::Y_UP) const;

    // Matrix access
    const glm::dmat4& GetEnuToEcefMatrix() const { return enu_to_ecef_; }
    const glm::dmat4& GetEcefToEnuMatrix() const { return ecef_to_enu_; }

    // Origin info
    double GeoOriginLon() const { return geo_origin_lon_; }
    double GeoOriginLat() const { return geo_origin_lat_; }
    double GeoOriginHeight() const { return geo_origin_height_; }

    // Geoid config
    void EnableGeoidCorrection(bool enabled) { geoid_config_.enabled = enabled; }
    bool IsGeoidCorrectionEnabled() const { return geoid_config_.enabled; }
    const GeoidConfig& GetGeoidConfig() const { return geoid_config_; }

    // Static utility methods
    static glm::dmat4 CalcEnuToEcefMatrix(double lon_deg, double lat_deg, double height);
    static glm::dvec3 CartographicToEcef(double lon_deg, double lat_deg, double height);
    static glm::dmat4 GetAxisTransformMatrix(UpAxis from, UpAxis to);

private:
    void InitializeWithGeoRef(const GeoReference& geo_ref);
    void CreateOGRTransform();
    double ApplyGeoidCorrection(double lat, double lon, double height) const;
    bool ShouldApplyGeoidCorrection() const;

    CoordinateSystem source_cs_;
    TransformMode mode_ = TransformMode::None;

    double geo_origin_lon_ = 0.0;
    double geo_origin_lat_ = 0.0;
    double geo_origin_height_ = 0.0;

    glm::dmat4 enu_to_ecef_{1.0};
    glm::dmat4 ecef_to_enu_{1.0};
    glm::dmat4 axis_transform_{1.0};

    struct OGRCTDeleter {
        void operator()(OGRCoordinateTransformation* pCT) const {
            if (pCT) OGRCoordinateTransformation::DestroyCT(pCT);
        }
    };
    std::unique_ptr<OGRCoordinateTransformation, OGRCTDeleter> ogr_transform_;

    GeoidConfig geoid_config_;
};

} // namespace coords
