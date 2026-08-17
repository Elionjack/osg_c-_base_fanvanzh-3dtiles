cmake_minimum_required(VERSION 3.15)
project(root_glb_optimizer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(basisu CONFIG REQUIRED)
find_package(draco CONFIG REQUIRED)
find_package(meshoptimizer CONFIG REQUIRED)
find_package(Stb REQUIRED)
find_path(TINYGLTF_INCLUDE_DIRS "tiny_gltf.h" REQUIRED)

add_executable(root_glb_optimizer main.cpp)
target_include_directories(root_glb_optimizer PRIVATE
    ${TINYGLTF_INCLUDE_DIRS}
    ${Stb_INCLUDE_DIR}
)
target_link_libraries(root_glb_optimizer PRIVATE
    basisu::basisu_encoder
    draco::draco
    meshoptimizer::meshoptimizer
)
if(WIN32)
    find_library(BASISU_OPENCL_LIBRARY NAMES OpenCL64 OpenCL)
    if(BASISU_OPENCL_LIBRARY)
        target_link_libraries(root_glb_optimizer PRIVATE ${BASISU_OPENCL_LIBRARY})
    endif()
endif()
if(MSVC)
    target_compile_definitions(root_glb_optimizer PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(root_glb_optimizer PRIVATE /Zc:preprocessor /bigobj)
endif()
