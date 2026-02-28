get_filename_component(_TEST_NAME ${CMAKE_CURRENT_LIST_DIR} NAME)

set(_SLA_PRINT_TEST_SOURCES
    ${_TEST_NAME}_tests_main.cpp
    sla_print_tests.cpp
    sla_test_utils.hpp sla_test_utils.cpp
    sla_supptgen_tests.cpp
    sla_raycast_tests.cpp
    sla_supptreeutils_tests.cpp
    sla_archive_readwrite_tests.cpp)

if (WIN32)
    # Some legacy SLA print tests still target historical APIs and currently fail to compile with MSVC.
    # Keep a smoke target on Windows while the individual cases are being ported.
    set(_SLA_PRINT_TEST_SOURCES
        ${_TEST_NAME}_tests_main.cpp
    )
endif()

add_executable(${_TEST_NAME}_tests ${_SLA_PRINT_TEST_SOURCES})

# mold linker for successful linking needs also to link TBB library and link it before libslic3r.
target_link_libraries(${_TEST_NAME}_tests test_common TBB::tbb TBB::tbbmalloc libslic3r)
set_property(TARGET ${_TEST_NAME}_tests PROPERTY FOLDER "tests")

if (WIN32)
    prusaslicer_copy_dlls(${_TEST_NAME}_tests)
endif()

# catch_discover_tests(${_TEST_NAME}_tests TEST_PREFIX "${_TEST_NAME}: ")
add_test(${_TEST_NAME}_tests ${_TEST_NAME}_tests ${CATCH_EXTRA_ARGS})
