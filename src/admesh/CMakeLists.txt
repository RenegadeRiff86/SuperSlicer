cmake_minimum_required(VERSION 3.10)
project(admesh)

add_library(admesh STATIC
    connect.cpp
    normals.cpp
    shared.cpp
    stl.h
    stl_io.cpp
    stlinit.cpp
    util.cpp
)

target_link_libraries(admesh PRIVATE boost_headeronly)

if (MSVC)
    # Suppress all compiler and code-analysis diagnostics from this bundled third-party library.
    target_compile_options(admesh PRIVATE /W0 /analyze-)
    set_target_properties(admesh PROPERTIES
        VS_GLOBAL_EnableCppCoreCheck false
        VS_GLOBAL_RunCodeAnalysis false
    )
endif ()
