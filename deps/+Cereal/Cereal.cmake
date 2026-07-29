add_cmake_project(Cereal
    URL "https://github.com/USCiLab/cereal/archive/refs/tags/v1.3.0.zip"
    URL_HASH SHA256=71642cb54658e98c8f07a0f0d08bf9766f1c3771496936f6014169d3726d9657
    PATCH_COMMAND patch --batch --forward -p1 -i ${CMAKE_CURRENT_LIST_DIR}/cereal-cxx23.patch
    CMAKE_ARGS
        -DJUST_INSTALL_CEREAL=ON
        -DSKIP_PERFORMANCE_COMPARISON=ON
        -DBUILD_TESTS=OFF
)