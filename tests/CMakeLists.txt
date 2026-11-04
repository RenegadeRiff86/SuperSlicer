# TODO Add individual tests as executables in separate directories
# add_subirectory(<testcase>)

find_package(Catch2 2.9 REQUIRED)

include(Catch)

set(TEST_DATA_DIR ${CMAKE_CURRENT_SOURCE_DIR}/data)
file(TO_NATIVE_PATH "${TEST_DATA_DIR}" TEST_DATA_DIR)

set(CATCH_EXTRA_ARGS "" CACHE STRING "Extra arguments for catch2 test suites.")

set(COMMON_DATA_UTILS
    ${CMAKE_CURRENT_SOURCE_DIR}/data/cpp/test_data.cpp
)


add_library(test_common INTERFACE)
target_include_directories(test_common INTERFACE ${CMAKE_CURRENT_LIST_DIR})
target_compile_definitions(test_common INTERFACE TEST_DATA_DIR=R"\(${TEST_DATA_DIR}\)" CATCH_CONFIG_FAST_COMPILE)
target_link_libraries(test_common INTERFACE Catch2::Catch2)

add_library(test_common_data ${COMMON_DATA_UTILS})
target_include_directories(test_common_data PUBLIC ${TEST_DATA_DIR}/include)
target_link_libraries(test_common_data PRIVATE test_common libslic3r)

if (APPLE)
    target_link_libraries(test_common INTERFACE "-liconv -framework IOKit" "-framework CoreFoundation" -lc++)
endif()

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

add_subdirectory(arrange)
add_subdirectory(thumbnails)
add_subdirectory(libslic3r)
add_subdirectory(superslicerlibslic3r)
add_subdirectory(fff_print)
add_subdirectory(sla_print)
add_subdirectory(cpp17 EXCLUDE_FROM_ALL)    # does not have to be built all the time

if (SLIC3R_GUI)
    add_subdirectory(slic3rutils)
endif()


# add_subdirectory(example)

# Bridge tongues and infill with nothing under it. Kept separate so it can be
# built without the legacy suites.
add_executable(bridge_support_tests
    bridge_support/bridge_support_tests.cpp
    bridge_support/test_bridge_support.cpp
)
target_link_libraries(bridge_support_tests test_common test_common_data libslic3r)
target_compile_definitions(bridge_support_tests PRIVATE
    BRIDGE_SUPPORT_MODEL="${CMAKE_SOURCE_DIR}/models/thin_walls_bowflex.stl")
set_property(TARGET bridge_support_tests PROPERTY FOLDER "tests")
add_test(bridge_support_tests bridge_support_tests)
