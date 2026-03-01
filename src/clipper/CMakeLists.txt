cmake_minimum_required(VERSION 3.10)
project(clipper)

add_library(clipper STATIC
# We are using ClipperLib compiled as part of the libslic3r project using Slic3r::Point as its base type.
#    clipper.cpp
#    clipper.hpp
    clipper_z.cpp
    clipper_z.hpp
)

target_link_libraries(clipper TBB::tbb TBB::tbbmalloc)

if (MSVC)
    # Suppress all compiler and code-analysis diagnostics from this bundled third-party library.
    target_compile_options(clipper PRIVATE /W0 /analyze-)
    set_target_properties(clipper PROPERTIES
        VS_GLOBAL_EnableCppCoreCheck false
        VS_GLOBAL_RunCodeAnalysis false
    )
endif ()
