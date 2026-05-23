cmake_minimum_required(VERSION 3.26)

project(updown-studio LANGUAGES CXX RC)

if(NOT WIN32)
    message(FATAL_ERROR "updown-studio currently supports Windows only.")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "updown-studio requires a 64-bit toolchain.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Fallback for developers who configure without a vcpkg toolchain preset.
# If the repository already contains a populated local vcpkg tree, reuse it.
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(_local_vcpkg_installed "${CMAKE_SOURCE_DIR}/vcpkg_installed/x64-windows")
    set(_local_vcpkg_prefix "${_local_vcpkg_installed}/x64-windows")
    if(EXISTS "${_local_vcpkg_prefix}/share/directxtk12/directxtk12-config.cmake")
        list(PREPEND CMAKE_PREFIX_PATH "${_local_vcpkg_prefix}")
        if(NOT DEFINED VCPKG_INSTALLED_DIR)
            set(VCPKG_INSTALLED_DIR "${_local_vcpkg_installed}" CACHE PATH "Local vcpkg installed root")
        endif()
        if(NOT DEFINED VCPKG_TARGET_TRIPLET)
            set(VCPKG_TARGET_TRIPLET "x64-windows" CACHE STRING "vcpkg target triplet")
        endif()
        message(STATUS "Using local vcpkg fallback: ${_local_vcpkg_prefix}")
    endif()
endif()

if(CMAKE_CONFIGURATION_TYPES)
    list(FIND CMAKE_CONFIGURATION_TYPES "Profile" _profile_index)
    if(_profile_index EQUAL -1)
        list(APPEND CMAKE_CONFIGURATION_TYPES Profile)
        set(CMAKE_CONFIGURATION_TYPES "${CMAKE_CONFIGURATION_TYPES}" CACHE STRING "Supported build types." FORCE)
    endif()
endif()

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

add_subdirectory(engine)
add_subdirectory(demo)

option(BUILD_SCENE_EXPORT "Build tools/SceneExport exporter tool" ON)
if(BUILD_SCENE_EXPORT)
    add_subdirectory(tools/SceneExport)
endif()
