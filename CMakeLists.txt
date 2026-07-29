#/|/ Copyright (c) Prusa Research 2017 - 2023 Tomáš Mészáros @tamasmeszaros, Vojtěch Bubník @bubnikv, Lukáš Matěna @lukasmatena, Filip Sykala @Jony01, Oleksandra Iushchenko @YuSanka, Lukáš Hejl @hejllukas, David Kocík @kocikdav, Enrico Turri @enricoturri1966, Vojtěch Král @vojtechkral
#/|/ Copyright (c) 2023 Ben Greiner
#/|/ Copyright (c) 2021 D-mo @dimitry-ishenko
#/|/ Copyright (c) 2020 Pascal de Bruijn @pmjdebruijn
#/|/ Copyright (c) 2019 Sam Segers
#/|/ Copyright (c) 2019 Colin Gilgenbach @hexane360
#/|/ Copyright (c) 2018 Dan Kortschak
#/|/
#/|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
#/|/
cmake_minimum_required(VERSION 3.13)

if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif()

project(Slic3r)

if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/version.date.inc")
	include("version.date.inc")
else()
	include("version.inc")
endif()
set(BOOST_VERBOSE NO)
include(GNUInstallDirs)
include(CMakeDependentOption)

set(SLIC3R_RESOURCES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/resources")
file(TO_NATIVE_PATH "${SLIC3R_RESOURCES_DIR}" SLIC3R_RESOURCES_DIR_WIN)

if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  message(STATUS "No build type selected, default to Release")
  set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type (default Release)" FORCE)
endif()

if(DEFINED ENV{SLIC3R_STATIC})
        set(SLIC3R_STATIC_INITIAL $ENV{SLIC3R_STATIC})
else()
        if (MSVC OR MINGW OR APPLE)
                set(SLIC3R_STATIC_INITIAL 1)
        else()
                set(SLIC3R_STATIC_INITIAL 0)
        endif()
endif()

if(NOT WIN32)
    # Static project libraries are also linked into OCCTWrapper, a shared module.
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)

    # Add DEBUG flags to debug builds.
    # Also add -g to RELWITHDEBINFO on non-win32
    set(_CC_DEBUG_FLAGS $<$<CONFIG:DEBUG>:-DwxDEBUG_LEVEL=1>
                        $<$<CONFIG:DEBUG>:-g>
                        $<$<CONFIG:RELWITHDEBINFO>:-g>
    )
endif()

option(SLIC3R_STATIC            "Compile Slic3r with static libraries (Boost, TBB, glew)" ${SLIC3R_STATIC_INITIAL})
option(SLIC3R_GUI               "Compile Slic3r with GUI components (OpenGL, wxWidgets)" 1)
option(SLIC3R_FHS               "Assume Slic3r is to be installed in a FHS directory structure" 0)
option(SLIC3R_PCH               "Use precompiled headers" 1)
option(SLIC3R_MSVC_COMPILE_PARALLEL "Compile on Visual Studio in parallel" 1)
option(SLIC3R_MSVC_PDB          "Generate PDB files on MSVC in Release mode" 1)
option(SLIC3R_ASAN              "Enable ASan on Clang and GCC" 0)
option(SLIC3R_UBSAN             "Enable UBSan on Clang and GCC" 0)
option(SLIC3R_ENABLE_FORMAT_STEP "Enable compilation of STEP file support" ON)
# If SLIC3R_FHS is 1 -> SLIC3R_DESKTOP_INTEGRATION is always 0, othrewise variable.
CMAKE_DEPENDENT_OPTION(SLIC3R_DESKTOP_INTEGRATION "Allow perfoming desktop integration during runtime" 1 "NOT SLIC3R_FHS" 0)

set(OPENVDB_FIND_MODULE_PATH "" CACHE PATH "Path to OpenVDB installation's find modules.")

set(SLIC3R_GTK "2" CACHE STRING "GTK version to use with wxWidgets on Linux")

set(IS_CROSS_COMPILE FALSE)

if (SLIC3R_STATIC)
    # Prefer config scripts over find modules. This is helpful when building with
    # the static dependencies. Many libraries have their own export scripts
    # while having a Find<PkgName> module in standard cmake installation.
    # (e.g. CURL)
    set(CMAKE_FIND_PACKAGE_PREFER_CONFIG ON)
endif ()

if (WIN32)
    # Optional Windows dependency discovery does not use pkg-config.
    set(CMAKE_DISABLE_FIND_PACKAGE_PkgConfig TRUE)
endif ()

# Dependency build management
option(${PROJECT_NAME}_BUILD_DEPS "Build dependencies before the project" OFF)
option(${PROJECT_NAME}_DEPS_OUTPUT_QUIET "Don't print build output for dependencies" OFF)
set(${PROJECT_NAME}_DEPS_PRESET "default" CACHE STRING "Preset of the dependencies when ${PROJECT_NAME}_BUILD_DEPS is ON")
set(${PROJECT_NAME}_DEPS_BUILD_DIR "" CACHE PATH "Binary dir of the dependencies build when ${PROJECT_NAME}_BUILD_DEPS is ON")
if (${PROJECT_NAME}_BUILD_DEPS)
    include(deps/autobuild.cmake)
endif ()

if (SLIC3R_STATIC)
    set(_slic3r_dependency_prefixes)
    if (${PROJECT_NAME}_DEPS_BUILD_DIR)
        list(APPEND _slic3r_dependency_prefixes "${${PROJECT_NAME}_DEPS_BUILD_DIR}/destdir/usr/local")
    endif()
    list(APPEND _slic3r_dependency_prefixes
        "${CMAKE_CURRENT_SOURCE_DIR}/deps/build-${${PROJECT_NAME}_DEPS_PRESET}/destdir/usr/local"
        "${CMAKE_CURRENT_SOURCE_DIR}/deps/build/destdir/usr/local"
    )

    foreach(_slic3r_dependency_prefix ${_slic3r_dependency_prefixes})
        if (EXISTS "${_slic3r_dependency_prefix}/lib/cmake/Boost-1.83.0" OR
            EXISTS "${_slic3r_dependency_prefix}/include/boost/version.hpp" OR
            EXISTS "${_slic3r_dependency_prefix}/include/boost-1_83/boost/version.hpp")
            list(FIND CMAKE_PREFIX_PATH "${_slic3r_dependency_prefix}" _slic3r_dependency_prefix_index)
            if (_slic3r_dependency_prefix_index EQUAL -1)
                list(PREPEND CMAKE_PREFIX_PATH "${_slic3r_dependency_prefix}")
                set(CMAKE_PREFIX_PATH "${CMAKE_PREFIX_PATH}" CACHE STRING "Package search prefixes" FORCE)
            endif()
            message(STATUS "Using bundled dependencies from ${_slic3r_dependency_prefix}")
            break()
        endif()
    endforeach()
endif()

if (APPLE)
    set(CMAKE_FIND_FRAMEWORK LAST)
    set(CMAKE_FIND_APPBUNDLE LAST)
    list(FIND CMAKE_OSX_ARCHITECTURES ${CMAKE_SYSTEM_PROCESSOR} _arch_idx)
    if (CMAKE_OSX_ARCHITECTURES AND _arch_idx LESS 0)
        set(IS_CROSS_COMPILE TRUE)
    endif ()
endif ()

option(SLIC3R_BUILD_SANDBOXES   "Build development sandboxes" OFF)
option(SLIC3R_BUILD_TESTS       "Build unit tests" OFF)

if (IS_CROSS_COMPILE)
    message("Detected cross compilation setup. Tests and encoding checks will be forcedly disabled!")
    set(SLIC3R_BUILD_TESTS OFF CACHE BOOL "" FORCE)
endif ()

# Print out the SLIC3R_* cache options
get_cmake_property(_cache_vars CACHE_VARIABLES)
list (SORT _cache_vars)
foreach (_cache_var ${_cache_vars})
    if("${_cache_var}" MATCHES "^SLIC3R_")
        message(STATUS "${_cache_var}: ${${_cache_var}}")
    endif ()
endforeach()

if (SLIC3R_GUI)
    add_definitions(-DSLIC3R_GUI)
endif ()

if(SLIC3R_DESKTOP_INTEGRATION)
    add_definitions(-DSLIC3R_DESKTOP_INTEGRATION)
endif ()

if (MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL Clang)
    set(IS_CLANG_CL TRUE)

    # clang-cl can interpret SYSTEM header paths if -imsvc is used
    set(CMAKE_INCLUDE_SYSTEM_FLAG_CXX "-imsvc")

    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall \
        -Wno-old-style-cast -Wno-reserved-id-macro -Wno-c++98-compat-pedantic")
else ()
    set(IS_CLANG_CL FALSE)
endif ()

if (MSVC)
    # Imported third-party packages often omit RelWithDebInfo/MinSizeRel artifacts.
    # Prefer their release variants for non-debug optimized builds to avoid mixed-runtime link noise.
    set(CMAKE_MAP_IMPORTED_CONFIG_RELWITHDEBINFO Release)
    set(CMAKE_MAP_IMPORTED_CONFIG_MINSIZEREL Release)
    foreach(_cfg RELEASE RELWITHDEBINFO MINSIZEREL)
        string(APPEND CMAKE_EXE_LINKER_FLAGS_${_cfg} " /LTCG")
        string(APPEND CMAKE_SHARED_LINKER_FLAGS_${_cfg} " /LTCG")
    endforeach()
endif ()

if (MSVC)
    if (SLIC3R_MSVC_COMPILE_PARALLEL AND NOT IS_CLANG_CL)
           add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:/MP>")
    endif ()
    # /bigobj (Increase Number of Sections in .Obj file)
    # error C3859: virtual memory range for PCH exceeded; please recompile with a command line option of '-Zm90' or greater
    # Generate symbols at every build target, even for the release.
    add_compile_options(
        "$<$<COMPILE_LANGUAGE:C,CXX>:-bigobj>"
        "$<$<COMPILE_LANGUAGE:C,CXX>:-Zm520>"
        "$<$<COMPILE_LANGUAGE:C,CXX>:/Zi>"
    )
    # Disable STL4007: Many result_type typedefs and all argument_type, first_argument_type, and second_argument_type typedefs are deprecated in C++17.
    #FIXME Remove this line after eigen library adapts to the new C++17 adaptor rules.
    add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:-D_SILENCE_CXX17_ADAPTOR_TYPEDEFS_DEPRECATION_WARNING>")
    # Enforce strict C++ conformance, so our code that compiles on MSVC also compiles on GCC and clang.
    add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:/permissive->")
    # Optimized MSVC links should not be incremental. The Windows jpeg-static dependency
    # also requests the static CRT, so prefer the target's /MD runtime for non-debug links.
    add_link_options(
        $<$<NOT:$<CONFIG:Debug>>:/INCREMENTAL:NO>
        $<$<NOT:$<CONFIG:Debug>>:/NODEFAULTLIB:LIBCMT>
    )
    # Leave CMAKE_INCLUDE_SYSTEM_FLAG_CXX/C unset so that CMake's VS generator
    # places SYSTEM include directories into the vcxproj <ExternalIncludePath>
    # property (VS-native mechanism).  IntelliSense Code Analysis reads
    # <ExternalIncludePath> to suppress CppCoreCheck/SAL diagnostics from
    # external headers; it does NOT read raw /external:I flags from
    # AdditionalOptions for that purpose.
    #
    # /external:W0     — suppress compiler warnings from external paths.
    # /analyze:external- — also suppress Code Analysis when /analyze is run
    #                      on the command line (MSVC 19.36+ / VS 2026).
    add_compile_options(
        "$<$<COMPILE_LANGUAGE:C,CXX>:/external:W0>"
        "$<$<COMPILE_LANGUAGE:C,CXX>:/analyze:external->"
    )
endif ()

if (MINGW)
    add_compile_options(-Wa,-mbig-obj)
endif ()

if (NOT MSVC)
    # ARMs (Raspberry PI) use an unsigned char by default. Let's make it consistent for PrusaSlicer on all platforms.
    add_compile_options(-fsigned-char)
endif ()

# Display and check CMAKE_PREFIX_PATH
message(STATUS "SLIC3R_STATIC: ${SLIC3R_STATIC}")
if (NOT "${CMAKE_PREFIX_PATH}" STREQUAL "")
    message(STATUS "CMAKE_PREFIX_PATH: ${CMAKE_PREFIX_PATH} (from cache or command line)")
    set(PREFIX_PATH_CHECK ${CMAKE_PREFIX_PATH})
elseif (NOT "$ENV{CMAKE_PREFIX_PATH}" STREQUAL "")
    message(STATUS "CMAKE_PREFIX_PATH: $ENV{CMAKE_PREFIX_PATH} (from environment)")
    set(PREFIX_PATH_CHECK $ENV{CMAKE_PREFIX_PATH})
else ()
    message(STATUS "CMAKE_PREFIX_PATH: (default)")
endif ()

foreach (DIR ${PREFIX_PATH_CHECK})
    if (NOT EXISTS "${DIR}")
        message(WARNING "CMAKE_PREFIX_PATH element doesn't exist: ${DIR}")
    endif ()
endforeach ()

# On MSVC, mark each CMAKE_PREFIX_PATH include directory as a system (external) include so
# that /external:W0 suppresses diagnostics from pre-built dependency headers.
if (MSVC)
    foreach (_dir ${CMAKE_PREFIX_PATH})
        if (IS_DIRECTORY "${_dir}/include")
            include_directories(SYSTEM "${_dir}/include")
        endif ()
    endforeach ()
endif ()

# Add our own cmake module path.
list(APPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake/modules/)

enable_testing ()

# Enable C++23 language standard.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# WIN10SDK_PATH is used to point CMake to the WIN10 SDK installation directory.
# We pick it from environment if it is not defined in another way
if(WIN32)
    if(NOT DEFINED WIN10SDK_PATH)
        if(DEFINED ENV{WIN10SDK_PATH})
                set(WIN10SDK_PATH "$ENV{WIN10SDK_PATH}")
        endif()
    endif()
    if(DEFINED WIN10SDK_PATH)
        if (EXISTS "${WIN10SDK_PATH}/include/winrt/windows.graphics.printing3d.h")
            set(WIN10SDK_INCLUDE_PATH "${WIN10SDK_PATH}/Include")
        else()
            message("WIN10SDK_PATH is invalid: ${WIN10SDK_PATH}")
            message("${WIN10SDK_PATH}/include/winrt/windows.graphics.printing3d.h was not found")
            message("STL fixing by WinSDK will not be compiled")
            unset(WIN10SDK_PATH)
        endif()
    else()
        # Try to use the default Windows 10 SDK path.
        if(NOT DEFINED ENV{WindowsSdkDir})
            # Query the registry for the canonical SDK root — more reliable than
            # guessing a Program Files path.
            get_filename_component(_kits_root
                "[HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows Kits\\Installed Roots;KitsRoot10]"
                ABSOLUTE CACHE)
            if(_kits_root AND EXISTS "${_kits_root}")
                set(CMAKE_WINDOWS_KITS_10_DIR "${_kits_root}")
            else()
                # Last-resort fallback: standard SDK install location.
                set(CMAKE_WINDOWS_KITS_10_DIR "C:/Program Files (x86)/Windows Kits/10")
            endif()
            message(STATUS "Env var 'WindowsSdkDir' not defined, trying location '${CMAKE_WINDOWS_KITS_10_DIR}' with version '${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}'.")
            set(WIN10SDK_PATH "${CMAKE_WINDOWS_KITS_10_DIR}")
            set(WIN10SDK_INCLUDE_PATH "${CMAKE_WINDOWS_KITS_10_DIR}/Include/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}")
            link_directories(${CMAKE_WINDOWS_KITS_10_DIR}/Lib/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/um/x64)
            if (NOT EXISTS "${WIN10SDK_INCLUDE_PATH}/winrt/windows.graphics.printing3d.h")
                message("${WIN10SDK_INCLUDE_PATH}/winrt/windows.graphics.printing3d.h was not found")
                message("STL fixing by the Netfabb service will not be compiled")
                unset(WIN10SDK_INCLUDE_PATH)
            endif()
        else()
			set(WIN10SDK_INCLUDE_PATH "$ENV{WindowsSdkDir}/Include/$ENV{WindowsSDKVersion}")
			if (NOT EXISTS "${WIN10SDK_INCLUDE_PATH}/winrt/windows.graphics.printing3d.h")
				message("${WIN10SDK_INCLUDE_PATH}/winrt/windows.graphics.printing3d.h was not found")
                message("STL fixing by WinSDK will not be compiled")
				unset(WIN10SDK_INCLUDE_PATH)
			endif()
		endif()
    endif()
    if(WIN10SDK_INCLUDE_PATH)
        message(STATUS "Building with Win10 STL fixing service support")
        add_definitions(-DHAS_WIN10SDK)
        include_directories("${WIN10SDK_INCLUDE_PATH}")
    else()
        message(STATUS "Building without Win10 STL fixing service support")
    endif()
endif()

if (APPLE)
    add_compile_options(-D_LIBCPP_ENABLE_CXX17_REMOVED_UNARY_BINARY_FUNCTION)
    message("OS X SDK Path: ${CMAKE_OSX_SYSROOT}")
    if (CMAKE_OSX_DEPLOYMENT_TARGET)
        message("OS X Deployment Target: ${CMAKE_OSX_DEPLOYMENT_TARGET}")
    else ()
        message("OS X Deployment Target: (default)")
    endif ()
endif ()

if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(PkgConfig REQUIRED)

    if (CMAKE_VERSION VERSION_LESS "3.1")
        # Compatibility fallback for very old CMake versions that do not honor
        # CMAKE_CXX_STANDARD.
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++23")
    endif()

    # Some Raspberry Pi Boost builds do not link pthreads automatically.
    set(THREADS_PREFER_PTHREAD_FLAG ON)
    find_package(Threads REQUIRED)

    if (SLIC3R_GUI)
        find_package(DBus REQUIRED)
        include_directories(${DBUS_INCLUDE_DIRS})
    endif ()
endif()

if (CMAKE_COMPILER_IS_GNUCC OR CMAKE_COMPILER_IS_GNUXX)
    # Adding -fext-numeric-literals to enable GCC extensions on definitions of quad float literals, which are required by Boost.
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fext-numeric-literals" )
endif()

# C++23 mode is available in GCC 11+ and Clang 13+. Fail early with a clear message.
if ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS "11")
    message(FATAL_ERROR "C++23 requires GCC 11 or later. Found GCC ${CMAKE_CXX_COMPILER_VERSION}.")
endif()
if ("${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS "13")
    message(FATAL_ERROR "C++23 requires Clang 13 or later. Found Clang ${CMAKE_CXX_COMPILER_VERSION}.")
endif()

if (NOT MSVC AND ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU" OR "${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang"))
    if (NOT MINGW)
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall" )
    endif ()
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-reorder" )

    # On GCC and Clang, no return from a non-void function is a warning only. Here, we make it an error.
    add_compile_options(-Werror=return-type)

    # Reduce noisy Eigen diagnostics. GCC supports this suppression starting with 6.1.
    # https://eigen.tuxfamily.org/bz/show_bug.cgi?id=1221
    if("${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang" OR CMAKE_CXX_COMPILER_VERSION VERSION_GREATER 6.0)
        add_compile_options(-Wno-ignored-attributes) # Tamas: Eigen include dirs are marked as SYSTEM
    endif()

    # Clang reports legacy OpenGL calls as deprecated. Keep this warning disabled
    # until the remaining deprecated OpenGL paths are removed.
    if("${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang")
        add_compile_options(-Wno-deprecated-declarations)
    endif()

    # Clang reports misleading indentation for some IF blocks because of mixing tabs with spaces.
    if("${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang")
        add_compile_options(-Wno-misleading-indentation)
    endif()

    # libigl triggers substantial -Wunknown-pragmas noise on GCC in practice.
    # Historical context:
    #   https://gcc.gnu.org/bugzilla/show_bug.cgi?id=66943
    #   https://gcc.gnu.org/bugzilla/show_bug.cgi?id=53431
    # Keep this warning disabled for GCC builds unless the local libigl/GCC
    # combination is verified to be clean without it.
    if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
        add_compile_options(-Wno-unknown-pragmas)
    endif()

endif()

if (SLIC3R_ASAN)
    # ASAN should be available on MSVC starting with Visual Studio 2019 16.9
    # https://devblogs.microsoft.com/cppblog/address-sanitizer-for-msvc-now-generally-available/
    add_compile_options(-fsanitize=address)

    if (NOT MSVC)
        add_compile_options(-fno-omit-frame-pointer)
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=address")
        set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -fsanitize=address")
        set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} -fsanitize=address")
    endif ()

    if ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -lasan")
    endif ()
endif ()

if (SLIC3R_UBSAN)
    # Stacktrace for every report is enabled by default. It can be disabled by running Slic3r with "UBSAN_OPTIONS=print_stacktrace=0".

    # Define macro SLIC3R_UBSAN to allow detection in the source code if this sanitizer is enabled.
    add_compile_definitions(SLIC3R_UBSAN)

    # Clang supports much more useful checks than GCC, so when Clang is detected, another checks will be enabled.
    # List of what GCC is checking: https://gcc.gnu.org/onlinedocs/gcc/Instrumentation-Options.html
    # List of what Clang is checking: https://clang.llvm.org/docs/UndefinedBehaviorSanitizer.html
    if ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang")
        set(_ubsan_flags "-fsanitize=undefined,integer")
    else ()
        set(_ubsan_flags "-fsanitize=undefined")
    endif ()

    add_compile_options(${_ubsan_flags} -fno-omit-frame-pointer)

    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${_ubsan_flags}")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} ${_ubsan_flags}")
    set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} ${_ubsan_flags}")

    if ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -lubsan")
    endif ()
endif ()

if (APPLE)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Werror=partial-availability -Werror=unguarded-availability -Werror=unguarded-availability-new")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Werror=partial-availability -Werror=unguarded-availability -Werror=unguarded-availability-new")
endif ()

# Where all the bundled libraries reside?
set(LIBDIR ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(LIBDIR_BIN ${CMAKE_CURRENT_BINARY_DIR}/src)
# For the bundled boost libraries (boost::nowide)
include_directories(${LIBDIR})
# For generated header files
include_directories(${LIBDIR_BIN}/platform)

if(WIN32)
    add_definitions(-D_USE_MATH_DEFINES -D_WIN32 -D_CRT_SECURE_NO_WARNINGS -D_SCL_SECURE_NO_WARNINGS)
    if(MSVC)
        # BOOST_ALL_NO_LIB: Avoid the automatic linking of Boost libraries on Windows. Rather rely on explicit linking.
        add_definitions(-DBOOST_ALL_NO_LIB -DBOOST_USE_WINAPI_VERSION=0x601 -DBOOST_SYSTEM_USE_UTF8 )
        # Force the source code encoding to UTF-8. See PrusaSlicer GH pull request #5583
        add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:/utf-8>")
        add_compile_definitions("$<$<CONFIG:RelWithDebInfo>:_RELWITHDEBINFO>")
    endif(MSVC)
endif(WIN32)

add_definitions(-DwxUSE_UNICODE -D_UNICODE -DUNICODE -DWXINTL_NO_GETTEXT_MACRO)

# Disable unsafe implicit wxString to const char* / std::string and vice versa. This implicit conversion breaks the UTF-8 encoding quite often.
add_definitions(-DwxNO_UNSAFE_WXSTRING_CONV)

# Find and configure boost
if(SLIC3R_STATIC)
    # Use static boost libraries.
    set(Boost_USE_STATIC_LIBS ON)
endif()
# Boost 1.66 is the first version that provides both boost::process and
# boost::beast::detail::base64.
set(MINIMUM_BOOST_VERSION "1.66.0")
set(_boost_components "filesystem;thread;log;locale;regex;chrono;atomic;date_time;iostreams;program_options")
# Boost.System has been header-only since 1.66, and current Boost (1.89+) no longer
# installs a boost_system CMake component config at all - so REQUIRED-ing it aborts the
# configure on an up-to-date distro. Keep it optional: older Boost still resolves the
# component, and the only consumer already guards with if(TARGET Boost::system).
find_package(Boost ${MINIMUM_BOOST_VERSION} REQUIRED COMPONENTS ${_boost_components} OPTIONAL_COMPONENTS system)
# Boost typically ships only Debug and Release binaries here, so map
# RelWithDebInfo to Release for imported libraries.
if (MSVC)
  set_target_properties(${Boost_LIBRARIES} PROPERTIES MAP_IMPORTED_CONFIG_RELWITHDEBINFO RELEASE)
endif()

add_library(boost_libs INTERFACE)
add_library(boost_headeronly INTERFACE)

if (APPLE)
    # Historical Boost.Asio kqueue workaround on macOS:
    # https://svn.boost.org/trac/boost/ticket/5339
    target_compile_definitions(boost_headeronly INTERFACE BOOST_ASIO_DISABLE_KQUEUE)
endif()

if(NOT SLIC3R_STATIC)
    target_compile_definitions(boost_headeronly INTERFACE BOOST_LOG_DYN_LINK)
endif()

function(slic3r_remap_configs targets from_Cfg to_Cfg)
    if(MSVC)
        string(TOUPPER ${from_Cfg} from_CFG)
    
        foreach(tgt ${targets})
            if(TARGET ${tgt})
                set_target_properties(${tgt} PROPERTIES MAP_IMPORTED_CONFIG_${from_CFG} ${to_Cfg})
            endif()
        endforeach()
    endif()
endfunction()

if(TARGET Boost::system)
    message(STATUS "Boost::boost exists")
    target_link_libraries(boost_headeronly INTERFACE Boost::boost)

    # Keep this explicit loop for compatibility with older CMake releases.
    set(_boost_targets "")
    foreach(comp ${_boost_components})
        list(APPEND _boost_targets "Boost::${comp}")
    endforeach()

    target_link_libraries(boost_libs INTERFACE
        boost_headeronly # includes the custom compile definitions as well
        ${_boost_targets}
        )
    slic3r_remap_configs("${_boost_targets}" RelWithDebInfo Release)
else()
    target_include_directories(boost_headeronly INTERFACE ${Boost_INCLUDE_DIRS})
    target_link_libraries(boost_libs INTERFACE boost_headeronly ${Boost_LIBRARIES})
endif()



# Find and configure intel-tbb
if(SLIC3R_STATIC)
    set(TBB_STATIC 1)
endif()
set(TBB_DEBUG 1)
find_package(TBB REQUIRED)
slic3r_remap_configs(TBB::tbb RelWithDebInfo Release)
slic3r_remap_configs(TBB::tbbmalloc RelWithDebInfo Release)

find_package(CURL REQUIRED)

add_library(libcurl INTERFACE)
target_link_libraries(libcurl INTERFACE CURL::libcurl)

if (SLIC3R_STATIC AND UNIX AND NOT APPLE)
    # Static Linux builds locate a usable CA bundle at first launch.
    target_compile_definitions(libcurl INTERFACE OPENSSL_CERT_OVERRIDE)
endif()

# Work around incomplete or inconsistent CURL CMake package linkage by
# attaching the extra platform libraries we rely on here.
if (NOT WIN32)
    # Required by libcurl
    find_package(ZLIB REQUIRED)
    target_link_libraries(libcurl INTERFACE ZLIB::ZLIB)
else()
    target_link_libraries(libcurl INTERFACE crypt32)
endif()

## OPTIONAL packages

# Find eigen3 or use bundled version
if (NOT SLIC3R_STATIC)
    find_package(Eigen3 3.3)
endif ()
if (NOT EIGEN3_FOUND)
    set(EIGEN3_FOUND 1)
    set(EIGEN3_INCLUDE_DIR ${LIBDIR}/eigen/)
endif ()
include_directories(BEFORE SYSTEM ${EIGEN3_INCLUDE_DIR})

# Find expat. We have our overriden FindEXPAT which exports libexpat target
# no matter what.
find_package(EXPAT REQUIRED)

add_library(libexpat INTERFACE)

if (TARGET EXPAT::EXPAT )
    target_link_libraries(libexpat INTERFACE EXPAT::EXPAT)
elseif(TARGET expat::expat)
    target_link_libraries(libexpat INTERFACE expat::expat)
endif ()

find_package(PNG REQUIRED)

set(OpenGL_GL_PREFERENCE "LEGACY")
find_package(OpenGL REQUIRED)

# Find glew or use bundled version
if (SLIC3R_STATIC AND NOT SLIC3R_STATIC_EXCLUDE_GLEW)
    set(GLEW_USE_STATIC_LIBS ON)
    set(GLEW_VERBOSE ON)
endif()

find_package(GLEW REQUIRED)

# Find the Cereal serialization library
find_package(cereal REQUIRED)
add_library(libcereal INTERFACE)
if (NOT TARGET cereal::cereal)
    target_link_libraries(libcereal INTERFACE cereal)
else()
    target_link_libraries(libcereal INTERFACE cereal::cereal)
endif()

# l10n
set(L10N_DIR "${SLIC3R_RESOURCES_DIR}/localization")
add_custom_target(gettext_make_pot
    COMMAND xgettext --keyword=L --keyword=_L --keyword=_u8L --keyword=L_CONTEXT:1,2c --keyword=_L_PLURAL:1,2 --add-comments=TRN --from-code=UTF-8 --debug --boost
        -f "${L10N_DIR}/list.txt"
        -o "${L10N_DIR}/Slic3r.pot"
    COMMAND hintsToPot ${SLIC3R_RESOURCES_DIR} ${L10N_DIR}
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    COMMENT "Generate pot file from strings in the source tree"
)

add_custom_target(gettext_merge_community_po_with_pot
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    COMMENT "Merge community po with new generated pot file"
)
file(GLOB L10N_PO_FILES "${L10N_DIR}/*/Slic3r*.po")
# Locale directories that PrusaSlicer already ships internally.
list(APPEND PS_L10N_DIRS "cs" "de" "es" "fr" "it" "ja" "pl")
foreach(po_file ${L10N_PO_FILES})
    GET_FILENAME_COMPONENT(po_dir "${po_file}" DIRECTORY)
    GET_FILENAME_COMPONENT(po_dir_name "${po_dir}" NAME)
    list(FIND PS_L10N_DIRS ${po_dir_name} found_dir_id)
    # found_dir_id == -1 means po_dir_name is community-managed here.
    if(found_dir_id LESS 0)
    add_custom_command(
            TARGET gettext_merge_community_po_with_pot PRE_BUILD
        COMMAND msgmerge -N -o ${po_file} ${po_file} "${L10N_DIR}/Slic3r.pot"
        # Remove obsolete entries before later merges with wxWidgets.po.
        COMMAND msgattrib --no-obsolete -o ${po_file} ${po_file}
    )
    endif()
endforeach()

add_custom_target(gettext_concat_wx_po_with_po
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    COMMENT "Concatenate and merge wxWidgets localization po with Slic3r po file"
)
file(GLOB L10N_PO_FILES "${L10N_DIR}/*/Slic3r*.po")
foreach(po_file ${L10N_PO_FILES})
    GET_FILENAME_COMPONENT(po_dir "${po_file}" DIRECTORY)
    GET_FILENAME_COMPONENT(po_dir_name "${po_dir}" NAME)
    SET(wx_po_file "${L10N_DIR}/wx_locale/${po_dir_name}.po")
    add_custom_command(
        TARGET gettext_concat_wx_po_with_po PRE_BUILD
        COMMAND msgcat --use-first -o ${po_file} ${po_file} ${wx_po_file}
        # Remove obsolete entries from the merged PO.
        COMMAND msgattrib --no-obsolete -o ${po_file} ${po_file}
    )
endforeach()

add_custom_target(gettext_po_to_mo
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    COMMENT "Generate localization mo files (binary) from po files (texts)"
)
file(GLOB L10N_PO_FILES "${L10N_DIR}/*/Slic3r.po")
foreach(po_file ${L10N_PO_FILES})
    GET_FILENAME_COMPONENT(po_dir "${po_file}" DIRECTORY)
    SET(mo_file "${po_dir}/${SLIC3R_APP_KEY}.mo")
    message(VERBOSE "po_dir mo_file imgui : '${po_file}' and mo file is '${mo_file}'")
    add_custom_command(
        TARGET gettext_po_to_mo PRE_BUILD
        COMMAND msgfmt ARGS --check-format -o ${mo_file} ${po_file}
        #COMMAND msgfmt ARGS --check-compatibility -o ${mo_file} ${po_file}
    )
endforeach()

find_package(NLopt 1.4 REQUIRED)
slic3r_remap_configs(NLopt::nlopt RelWithDebInfo Release)

if(SLIC3R_STATIC)
    set(OPENVDB_USE_STATIC_LIBS ON)
    set(USE_BLOSC TRUE)
endif ()

find_package(OpenVDB 5.0 COMPONENTS openvdb)
if(OpenVDB_FOUND)
    slic3r_remap_configs(IlmBase::Half RelWithDebInfo Release)
    slic3r_remap_configs(Blosc::blosc RelWithDebInfo Release)
else ()
    message(FATAL_ERROR "OpenVDB could not be found with the bundled find module. "
                   "You can try to specify the find module location of your "
                   "OpenVDB installation with the OPENVDB_FIND_MODULE_PATH cache variable.")
endif ()

if (NOT WIN32)
    find_path(SPNAV_INCLUDE_DIR spnav.h)
    if (SPNAV_INCLUDE_DIR)
        find_library(HAVE_SPNAV spnav)
        if (HAVE_SPNAV)
            add_definitions(-DHAVE_SPNAV)
            add_library(libspnav SHARED IMPORTED)
            target_link_libraries(libspnav INTERFACE spnav)
            message(STATUS "SPNAV library found")
        else()
            message(STATUS "SPNAV library NOT found, Spacenavd not supported")
        endif()
    else()
        message(STATUS "SPNAV library NOT found, Spacenavd not supported")
    endif()
endif()

set(TOP_LEVEL_PROJECT_DIR ${PROJECT_SOURCE_DIR})
function(prusaslicer_copy_dlls target)
    if ("${CMAKE_SIZEOF_VOID_P}" STREQUAL "8")
        set(_bits 64)
    elseif ("${CMAKE_SIZEOF_VOID_P}" STREQUAL "4")
        set(_bits 32)
    endif ()
    
    get_property(_is_multi GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
    get_target_property(_alt_out_dir ${target} RUNTIME_OUTPUT_DIRECTORY)

    if (_alt_out_dir)
        set (_out_dir "${_alt_out_dir}")
    elseif (_is_multi)
        set (_out_dir "$<TARGET_PROPERTY:${target},BINARY_DIR>/$<CONFIG>")
    else ()
        set (_out_dir "$<TARGET_PROPERTY:${target},BINARY_DIR>")
    endif ()
    
    # Keep this as a separate target to avoid Windows command-line length limits.
    add_custom_command(TARGET ${target} POST_BUILD 
        COMMAND ${CMAKE_COMMAND} -E copy ${TOP_LEVEL_PROJECT_DIR}/deps/+GMP/gmp/lib/win${_bits}/libgmp-10.dll ${_out_dir}
        COMMENT "Copy gmp runtime to build tree"
        VERBATIM)
    
    add_custom_command(TARGET ${target} POST_BUILD 
        COMMAND ${CMAKE_COMMAND} -E copy ${TOP_LEVEL_PROJECT_DIR}/deps/+MPFR/mpfr/lib/win${_bits}/libmpfr-4.dll ${_out_dir}
        COMMENT "Copy mpfr runtime to build tree"
        VERBATIM)

endfunction()


# libslic3r, Slic3r GUI and the Slic3r executable.
add_subdirectory(src)
set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT Slic3r_app_console)

add_dependencies(gettext_make_pot hintsToPot)

# Slice-verification tooling (scripts/slice_verify.py, doc/slice-verification.md).
# Attached here rather than added to the solution by hand so it survives
# regeneration -- a CMake-generated solution discards manual edits. Requires the
# Visual Studio Python workload; skipped silently when it is not installed, since
# an unloadable project would be worse than an absent one.
if (MSVC AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/scripts/slice_verify.pyproj")
    include_external_msproject(slice_verify
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/slice_verify.pyproj"
        TYPE 888888A0-9F3D-457C-B088-3A5042F75D52
        GUID 6F3C1D9A-4B87-4E2A-9C5E-2B7A8D1F4E60)
    set_target_properties(slice_verify PROPERTIES EXCLUDE_FROM_DEFAULT_BUILD TRUE)
endif ()

if(SLIC3R_BUILD_SANDBOXES)
    add_subdirectory(sandboxes)
endif()

if(SLIC3R_BUILD_TESTS)
    add_subdirectory(tests)
endif()

if (NOT WIN32 AND NOT APPLE)
    configure_file(${LIBDIR}/platform/unix/build_appimage.sh.in ${LIBDIR_BIN}/build_appimage.sh @ONLY)
endif()

# Resources install target, configure fhs.hpp on UNIX
if (WIN32)
    install(DIRECTORY "${SLIC3R_RESOURCES_DIR}/" DESTINATION "${CMAKE_INSTALL_PREFIX}/resources")
elseif (SLIC3R_FHS)
    # CMAKE_INSTALL_FULL_DATAROOTDIR: read-only architecture-independent data root (share)
    set(SLIC3R_FHS_RESOURCES "${CMAKE_INSTALL_FULL_DATAROOTDIR}/${SLIC3R_APP_KEY}")
    install(DIRECTORY ${SLIC3R_RESOURCES_DIR}/ DESTINATION ${SLIC3R_FHS_RESOURCES}
        PATTERN "*/udev" EXCLUDE
    )
    configure_file(${LIBDIR}/platform/unix/Slic3r.desktop.in ${LIBDIR_BIN}/${SLIC3R_APP_KEY}.desktop @ONLY)
    configure_file(${LIBDIR}/platform/unix/Gcodeviewer.desktop.in ${LIBDIR_BIN}/${SLIC3R_APP_KEY}-Gcodeviewer.desktop @ONLY)
    install(FILES ${LIBDIR_BIN}/${SLIC3R_APP_KEY}.desktop DESTINATION ${CMAKE_INSTALL_FULL_DATAROOTDIR}/applications)
    install(FILES ${LIBDIR_BIN}/${SLIC3R_APP_KEY}-Gcodeviewer.desktop DESTINATION ${CMAKE_INSTALL_FULL_DATAROOTDIR}/applications)
    foreach(SIZE 32 128 192)
        install(FILES ${SLIC3R_RESOURCES_DIR}/icons/${SLIC3R_APP_KEY}_${SIZE}px.png
            DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/${SIZE}x${SIZE}/apps RENAME ${SLIC3R_APP_KEY}.png
        )
        install(FILES ${SLIC3R_RESOURCES_DIR}/icons/${SLIC3R_APP_KEY}-gcodeviewer_${SIZE}px.png
            DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/${SIZE}x${SIZE}/apps RENAME ${SLIC3R_APP_KEY}-gcodeviewer.png
        )
    endforeach()
    install(DIRECTORY ${SLIC3R_RESOURCES_DIR}/udev/ DESTINATION lib/udev/rules.d)
else ()
    configure_file(${LIBDIR}/platform/unix/Slic3r.desktop.in ${LIBDIR_BIN}/${SLIC3R_APP_KEY}.desktop @ONLY)
    configure_file(${LIBDIR}/platform/unix/Gcodeviewer.desktop.in ${LIBDIR_BIN}/${SLIC3R_APP_KEY}-Gcodeviewer.desktop @ONLY)
    install(FILES ${LIBDIR_BIN}/${SLIC3R_APP_KEY}.desktop DESTINATION ${CMAKE_INSTALL_PREFIX}/resources/applications)
    install(FILES ${LIBDIR_BIN}/${SLIC3R_APP_KEY}-Gcodeviewer.desktop DESTINATION ${CMAKE_INSTALL_PREFIX}/resources/applications)
    install(DIRECTORY "${SLIC3R_RESOURCES_DIR}/" DESTINATION "${CMAKE_INSTALL_PREFIX}/resources")
endif ()

configure_file(${LIBDIR}/platform/unix/fhs.hpp.in ${LIBDIR_BIN}/platform/unix/fhs.hpp)

if (NOT WIN32)
    set(CPACK_GENERATOR "STGZ;TGZ;TZ")
    set(CPACK_NSIS_DISPLAY_NAME "${SLIC3R_APP_NAME} ${SLIC3R_VERSION}")
    set(CPACK_OUTPUT_CONFIG_FILE "${CMAKE_SOURCE_DIR}/cmake/CPackConfig.cmake")
    set(CPACK_PACKAGE_DESCRIPTION_FILE "${CMAKE_CURRENT_SOURCE_DIR}/README.md")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "CMake is a build tool")
    set(CPACK_PACKAGE_EXECUTABLES "${SLIC3R_APP_CMD}")
    set(CPACK_PACKAGE_FILE_NAME "${SLIC3R_APP_KEY}-${SLIC3R_VERSION}-Linux-x86_64")
    set(CPACK_PACKAGE_INSTALL_DIRECTORY "${SLIC3R_APP_KEY} ${SLIC3R_VERSION}")
    set(CPACK_PACKAGE_INSTALL_REGISTRY_KEY "${SLIC3R_BUILD_ID}")
    set(CPACK_PACKAGE_NAME "${SLIC3R_APP_KEY}")
    set(CPACK_PACKAGE_VENDOR "${SLIC3R_APP_NAME}")
    set(CPACK_PACKAGE_VERSION "${SLIC3R_VERSION_FULL}")
    set(CPACK_PACKAGE_VERSION_MAJOR "2")
    set(CPACK_PACKAGE_VERSION_MINOR "0")
    set(CPACK_PACKAGE_VERSION_PATCH "0")
    set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
    set(CPACK_RESOURCE_FILE_README "${CMAKE_CURRENT_SOURCE_DIR}/README.md")
    set(CPACK_RESOURCE_FILE_WELCOME "${CMAKE_CURRENT_SOURCE_DIR}/README.md")
    set(CPACK_SOURCE_GENERATOR "TGZ;TZ")
    set(CPACK_SOURCE_PACKAGE_FILE_NAME "${SLIC3R_APP_KEY}-${SLIC3R_VERSION_FULL}")
    set(CPACK_SOURCE_STRIP_FILES "")
    set(CPACK_STRIP_FILES "bin/${SLIC3R_APP_CMD}")
    set(CPACK_SYSTEM_NAME "Linux-x86_64")
    set(CPACK_TOPLEVEL_TAG "Linux-x86_64")
endif()

include(CPack)
