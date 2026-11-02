add_cmake_project(JPEG
    URL https://github.com/libjpeg-turbo/libjpeg-turbo/archive/refs/tags/3.0.1.zip
    URL_HASH SHA256=d6d99e693366bc03897677650e8b2dfa76b5d6c54e2c9e70c03f0af821b0a52f
    CMAKE_ARGS
        -DENABLE_SHARED=OFF
        -DENABLE_STATIC=ON
        # libjpeg-turbo's GNUInstallDirs resolves to lib64 on this layout while every other
        # dep installs to lib/. FindJPEG only looks in lib/, so without this it silently
        # falls back to the system libjpeg while still using these headers - a v62 header /
        # v80 library mismatch that corrupts jpeg_decompress_struct and crashes wxJPEGHandler.
        # Typed as STRING on purpose: as a PATH, CMake resolves a relative value against
        # the build directory and the install lands outside destdir entirely.
        -DCMAKE_INSTALL_LIBDIR:STRING=lib
)

set(DEP_JPEG_DEPENDS ZLIB)
