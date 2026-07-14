# veroutines - header-only discrete-event simulation kernel on C++ coroutines.
# CMake-first: consumers do find_package(veroutines) or FetchContent, then
# link veroutines::veroutines. Tests and demo build only when veroutines is
# the top-level project (toggle with VEROUTINES_BUILD_TESTS / _DEMO).

cmake_minimum_required(VERSION 3.20)

project(veroutines
    VERSION 0.1.0
    DESCRIPTION "Discrete-event simulation kernel on C++ coroutines"
    HOMEPAGE_URL "https://github.com/nohous/veroutines"
    LANGUAGES CXX)

# ---- the library ------------------------------------------------------------

add_library(veroutines INTERFACE)
add_library(veroutines::veroutines ALIAS veroutines)

target_compile_features(veroutines INTERFACE cxx_std_23)
target_include_directories(veroutines INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")

# ---- options ----------------------------------------------------------------

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(veroutines_is_top ON)
else()
    set(veroutines_is_top OFF)
endif()

option(VEROUTINES_BUILD_TESTS    "Build the veroutines test suite" ${veroutines_is_top})
option(VEROUTINES_BUILD_DEMO     "Build the veroutines demo"       ${veroutines_is_top})
option(VEROUTINES_BUILD_EXAMPLES "Build the veroutines examples"   ${veroutines_is_top})

if(VEROUTINES_BUILD_TESTS OR VEROUTINES_BUILD_EXAMPLES)
    enable_testing()
endif()

if(VEROUTINES_BUILD_TESTS)
    add_subdirectory(tests)
endif()

if(VEROUTINES_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(VEROUTINES_BUILD_DEMO)
    add_subdirectory(demo)
endif()

# ---- install / export -------------------------------------------------------

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(TARGETS veroutines EXPORT veroutines-targets)

install(EXPORT veroutines-targets
    FILE veroutines-targets.cmake
    NAMESPACE veroutines::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/veroutines")

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/veroutines-config.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/veroutines-config.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/veroutines")

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/veroutines-config-version.cmake"
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY SameMajorVersion
    ARCH_INDEPENDENT)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/veroutines-config.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/veroutines-config-version.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/veroutines")
