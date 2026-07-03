cmake_minimum_required(VERSION 3.15)
project(OsgbTo3dTiles)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ============================================================
# Source files
# ============================================================
set(SOURCES
    src/main.cpp
    src/osgb_converter.cpp
    src/osg_gltf_converter.cpp
    src/coordinate_system.cpp
    src/coordinate_transformer.cpp
    src/geoid_height.cpp
    src/mesh_processor.cpp
)

set(HEADERS
    src/utils.h
    src/osgb_converter.h
    src/osg_gltf_converter.h
    src/coordinate_system.h
    src/coordinate_transformer.h
    src/geoid_height.h
    src/mesh_processor.h
)

# ============================================================
# Find required packages via vcpkg
# ============================================================

# JSON library
find_package(nlohmann_json CONFIG REQUIRED)

# OpenSceneGraph
find_package(OpenSceneGraph REQUIRED COMPONENTS osg osgDB osgUtil OpenThreads)

# GDAL (for coordinate transformation)
find_package(GDAL REQUIRED)

# glm (math library)
find_package(glm CONFIG REQUIRED)

# Eigen3 (linear algebra for SVD)
find_package(Eigen3 CONFIG REQUIRED)

# tinygltf (glTF 2.0 writing)
find_path(TINYGLTF_INCLUDE_DIRS "tiny_gltf.h")
if(NOT TINYGLTF_INCLUDE_DIRS)
    message(FATAL_ERROR "tiny_gltf.h not found. Install via vcpkg: ./vcpkg install tinygltf")
endif()

# GeographicLib (geoid height correction)
find_package(GeographicLib CONFIG REQUIRED)

# Optional: basisu (KTX2 texture compression)
find_package(basisu CONFIG QUIET)
if(basisu_FOUND)
    message(STATUS "basisu found: KTX2 texture compression enabled")
    add_compile_definitions(HAS_BASISU)
else()
    message(WARNING "basisu not found - KTX2 compression disabled")
endif()

# Optional: Draco (mesh compression)
find_package(draco CONFIG QUIET)
if(draco_FOUND)
    message(STATUS "Draco found: mesh compression enabled")
    add_compile_definitions(HAS_DRACO)
else()
    message(WARNING "Draco not found - mesh compression disabled")
endif()

# Optional: meshoptimizer (mesh simplification)
find_package(meshoptimizer CONFIG QUIET)
if(meshoptimizer_FOUND)
    message(STATUS "meshoptimizer found: mesh simplification enabled")
    add_compile_definitions(HAS_MESHOPTIMIZER)
else()
    message(WARNING "meshoptimizer not found - mesh simplification disabled")
endif()

# Optional: stb (image I/O)
find_package(Stb QUIET)
if(Stb_FOUND)
    add_compile_definitions(HAS_STB)
endif()

# ============================================================
# Build executable
# ============================================================
add_executable(osgb_converter ${SOURCES} ${HEADERS})

target_include_directories(osgb_converter PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${GDAL_INCLUDE_DIRS}
    ${OPENSCENEGRAPH_INCLUDE_DIRS}
    ${TINYGLTF_INCLUDE_DIRS}
    ${EIGEN3_INCLUDE_DIRS}
)

# Link required libraries
target_link_libraries(osgb_converter PRIVATE
    nlohmann_json::nlohmann_json
    ${GDAL_LIBRARIES}
    ${OPENSCENEGRAPH_LIBRARIES}
    glm::glm-header-only
    Eigen3::Eigen
    ${GeographicLib_LIBRARIES}
)

if(basisu_FOUND)
    target_link_libraries(osgb_converter PRIVATE basisu::basisu_encoder)
endif()

if(draco_FOUND)
    target_link_libraries(osgb_converter PRIVATE draco::draco)
endif()

if(meshoptimizer_FOUND)
    target_link_libraries(osgb_converter PRIVATE meshoptimizer::meshoptimizer)
endif()

# Windows-specific settings
if(WIN32)
    target_compile_definitions(osgb_converter PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(osgb_converter PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/Zc:preprocessor /bigobj>)
endif()

# macOS-specific settings
if(APPLE)
    set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
    set(CMAKE_INSTALL_RPATH "@loader_path")
    set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)
endif()

