get_filename_component(_TEST_NAME ${CMAKE_CURRENT_LIST_DIR} NAME)

set(_SLIC3RUTILS_TEST_SOURCES
    ${_TEST_NAME}_tests_main.cpp
    slic3r_jobs_tests.cpp
    slic3r_version_tests.cpp
    slic3r_arrangejob_tests.cpp
    )

if (WIN32)
    # The arrange-job coverage is currently ahead of the Windows test harness API surface.
    # Keep a smoke target on Windows while the stale cases are being ported.
    set(_SLIC3RUTILS_TEST_SOURCES
        ${_TEST_NAME}_tests_main.cpp
        slic3r_jobs_tests.cpp
        slic3r_version_tests.cpp
    )
endif()

add_executable(${_TEST_NAME}_tests ${_SLIC3RUTILS_TEST_SOURCES})

if (SLIC3R_ENABLE_AUTOMATION_API)
    set(_AUTOMATION_SECURITY_TEST_NAME superslicer_automation_security_tests)
    add_executable(${_AUTOMATION_SECURITY_TEST_NAME}
        automation_tests_main.cpp
        automation_security_tests.cpp
        ../../src/slic3r/GUI/Automation/AutomationSecurity.cpp
    )
    target_include_directories(${_AUTOMATION_SECURITY_TEST_NAME} PRIVATE ../../src)
    target_link_libraries(${_AUTOMATION_SECURITY_TEST_NAME} test_common)
    set_property(TARGET ${_AUTOMATION_SECURITY_TEST_NAME} PROPERTY FOLDER "tests")
    add_test(NAME ${_AUTOMATION_SECURITY_TEST_NAME}
             COMMAND ${_AUTOMATION_SECURITY_TEST_NAME})
endif()

# mold linker for successful linking needs also to link TBB library and link it before libslic3r.
target_link_libraries(${_TEST_NAME}_tests test_common TBB::tbb TBB::tbbmalloc libslic3r_gui libslic3r)

if (WIN32)
    # Mirror the WebView2 loader lookup used by the main app targets so libslic3r_gui can link.
    get_filename_component(_deps_build_dir "${CMAKE_PREFIX_PATH}/../../../" ABSOLUTE)
    file(GLOB _webview2_pkg_dirs "${_deps_build_dir}/builds/wxWidgets/packages/Microsoft.Web.WebView2.*/build/native/x64")
    if (_webview2_pkg_dirs)
        list(GET _webview2_pkg_dirs 0 _webview2_lib_dir)
        target_link_directories(${_TEST_NAME}_tests PRIVATE ${_webview2_lib_dir})
    endif()
endif()

if (MSVC)
    target_link_libraries(${_TEST_NAME}_tests Setupapi.lib)
endif ()
set_property(TARGET ${_TEST_NAME}_tests PROPERTY FOLDER "tests")

if (WIN32)
    prusaslicer_copy_dlls(${_TEST_NAME}_tests)
endif()

# catch_discover_tests(${_TEST_NAME}_tests TEST_PREFIX "${_TEST_NAME}: ")
set(_catch_args "exclude:[NotWorking];-s")
list(APPEND _catch_args "${CATCH_EXTRA_ARGS}")
add_test(${_TEST_NAME}_tests ${_TEST_NAME}_tests ${_catch_args})
