#pragma once

#include <string>
#include <memory>
#include <optional>
#include <atomic>

// Forward declaration for GeographicLib
namespace GeographicLib { class Geoid; }

namespace GeoidHeight {

enum class GeoidModel {
    NONE,
    EGM84,
    EGM96,
    EGM2008
};

class GeoidCalculator {
public:
    GeoidCalculator() = default;
    ~GeoidCalculator() = default;

    GeoidCalculator(const GeoidCalculator&) = delete;
    GeoidCalculator& operator=(const GeoidCalculator&) = delete;

    bool Initialize(GeoidModel model, const std::string& geoidPath = "");
    bool IsInitialized() const { return geoid_ != nullptr; }
    GeoidModel GetModel() const { return model_.load(); }

    std::optional<double> GetGeoidHeight(double lat, double lon) const;
    double ConvertOrthometricToEllipsoidal(double lat, double lon, double orthometricHeight) const;
    double ConvertEllipsoidalToOrthometric(double lat, double lon, double ellipsoidalHeight) const;

    static std::string GeoidModelToString(GeoidModel model);
    static GeoidModel StringToGeoidModel(const std::string& str);
    static std::string GetDefaultGeoidDataPath();

private:
    std::atomic<GeoidModel> model_{GeoidModel::NONE};
    std::shared_ptr<GeographicLib::Geoid> geoid_;
};

GeoidCalculator& GetGlobalGeoidCalculator();
bool InitializeGlobalGeoidCalculator(GeoidModel model, const std::string& geoidPath = "");

} // namespace GeoidHeight
