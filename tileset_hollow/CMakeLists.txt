cmake_minimum_required(VERSION 3.15)
project(tileset_hollow)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# nlohmann_json (same vcpkg dependency as parent project)
find_package(nlohmann_json CONFIG REQUIRED)

add_executable(tileset_hollow
    main.cpp
)

target_link_libraries(tileset_hollow PRIVATE nlohmann_json::nlohmann_json)
target_include_directories(tileset_hollow PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

if(WIN32)
    target_compile_definitions(tileset_hollow PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(tileset_hollow PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/Zc:preprocessor>)
endif()
