cmake_minimum_required(VERSION 3.15)
project(tileset_json_optimizer)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(nlohmann_json CONFIG REQUIRED)

add_executable(tileset_json_optimizer main.cpp)
target_link_libraries(tileset_json_optimizer PRIVATE nlohmann_json::nlohmann_json)

if(WIN32)
    target_compile_definitions(tileset_json_optimizer PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(tileset_json_optimizer PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/Zc:preprocessor>)
endif()
