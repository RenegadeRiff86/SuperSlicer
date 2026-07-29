
set(_conf_cmd "./config")
set(_cross_arch "")
set(_cross_comp_prefix_line "")
if (CMAKE_CROSSCOMPILING)
    set(_conf_cmd "./Configure")
    set(_cross_comp_prefix_line "--cross-compile-prefix=${TOOLCHAIN_PREFIX}-")

    if (${CMAKE_SYSTEM_PROCESSOR} STREQUAL "aarch64" OR ${CMAKE_SYSTEM_PROCESSOR} STREQUAL "arm64")
        set(_cross_arch "linux-aarch64")
    elseif (${CMAKE_SYSTEM_PROCESSOR} STREQUAL "armhf") # For raspbian
        # TODO: verify
        set(_cross_arch "linux-armv4")
    endif ()
endif ()

ExternalProject_Add(dep_OpenSSL
    EXCLUDE_FROM_ALL ON
    # OpenSSL 3.5 is the supported LTS line through April 2030.
    URL "https://github.com/openssl/openssl/releases/download/openssl-3.5.7/openssl-3.5.7.tar.gz"
    URL_HASH SHA256=a8c0d28a529ca480f9f36cf5792e2cd21984552a3c8e4aa11a24aa31aeac98e8
    DOWNLOAD_DIR ${${PROJECT_NAME}_DEP_DOWNLOAD_DIR}/OpenSSL
    BUILD_IN_SOURCE ON
    CONFIGURE_COMMAND ${_conf_cmd} ${_cross_arch}
        "--prefix=${${PROJECT_NAME}_DEP_INSTALL_PREFIX}"
        "--libdir=lib"
        ${_cross_comp_prefix_line}
        no-shared
        no-ssl3-method
        no-dynamic-engine
        -Wa,--noexecstack
    BUILD_COMMAND make depend && make "-j${DEP_MAX_THREADS}"
    INSTALL_COMMAND make install_sw
)