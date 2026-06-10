include(FetchContent)

set(FETCHCONTENT_UPDATES_DISCONNECTED ON CACHE BOOL "Disable automatic dependency updates" FORCE)

# Captured at include time so the patch script resolves relative to this file.
set(UPDOWN_DEPS_PATCH_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/patches/fix_compileshaders_cwd.cmake")

function(updown_setup_dependencies)
    if(TARGET updown_deps_ready)
        return()
    endif()

    # Latest stable tags verified on 2026-05-24.
    FetchContent_Declare(
        directx_headers
        GIT_REPOSITORY https://github.com/microsoft/DirectX-Headers.git
        GIT_TAG v1.619.1
        GIT_SHALLOW TRUE
    )
    FetchContent_Declare(
        directxtk12
        GIT_REPOSITORY https://github.com/microsoft/DirectXTK12.git
        GIT_TAG mar2026
        GIT_SHALLOW TRUE
        PATCH_COMMAND ${CMAKE_COMMAND} -P "${UPDOWN_DEPS_PATCH_SCRIPT}"
    )
    FetchContent_Declare(
        directxtex
        GIT_REPOSITORY https://github.com/microsoft/DirectXTex.git
        GIT_TAG mar2026
        GIT_SHALLOW TRUE
        PATCH_COMMAND ${CMAKE_COMMAND} -P "${UPDOWN_DEPS_PATCH_SCRIPT}"
    )
    FetchContent_Declare(
        imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG v1.92.8
        GIT_SHALLOW TRUE
    )
    FetchContent_Declare(
        tracy
        GIT_REPOSITORY https://github.com/wolfpld/tracy.git
        GIT_TAG v0.13.1
        GIT_SHALLOW TRUE
    )
    FetchContent_Declare(
        assimp
        GIT_REPOSITORY https://github.com/assimp/assimp.git
        GIT_TAG v6.0.5
        GIT_SHALLOW TRUE
    )
    # Latest non-preview DXC release.
    FetchContent_Declare(
        dxc_bin
        URL https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.9.2602/dxc_2026_02_20.zip
    )

    set(ASSIMP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(ASSIMP_BUILD_ASSIMP_TOOLS OFF CACHE BOOL "" FORCE)
    set(ASSIMP_BUILD_ZLIB ON CACHE BOOL "" FORCE)
    set(ASSIMP_INSTALL OFF CACHE BOOL "" FORCE)

    FetchContent_MakeAvailable(directx_headers directxtk12 directxtex tracy assimp)
    FetchContent_MakeAvailable(imgui)
    FetchContent_MakeAvailable(dxc_bin)

    # Tracy's upstream CMake exports TRACY_ENABLE as a public definition.
    # Keep profiling opt-in at project targets (Profile config only).
    if(TARGET TracyClient)
        set_property(TARGET TracyClient PROPERTY INTERFACE_COMPILE_DEFINITIONS "")
    endif()

    # ImGui target (core + Win32 + DX12 backends).
    add_library(updown_imgui STATIC
        "${imgui_SOURCE_DIR}/imgui.cpp"
        "${imgui_SOURCE_DIR}/imgui_demo.cpp"
        "${imgui_SOURCE_DIR}/imgui_draw.cpp"
        "${imgui_SOURCE_DIR}/imgui_tables.cpp"
        "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
        "${imgui_SOURCE_DIR}/backends/imgui_impl_dx12.cpp"
        "${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp"
    )
    target_include_directories(updown_imgui PUBLIC
        "${imgui_SOURCE_DIR}"
        "${imgui_SOURCE_DIR}/backends"
    )
    target_compile_definitions(updown_imgui PUBLIC IMGUI_IMPL_API=)
    add_library(updown::imgui ALIAS updown_imgui)

    # Normalize assimp target naming.
    if(TARGET assimp AND NOT TARGET assimp::assimp)
        add_library(assimp::assimp ALIAS assimp)
    endif()

    # Normalize DirectX target naming (source-build names vary by project version).
    if(TARGET DirectXTK12 AND NOT TARGET Microsoft::DirectXTK12)
        add_library(Microsoft::DirectXTK12 ALIAS DirectXTK12)
    endif()
    if(TARGET DirectXTex AND NOT TARGET Microsoft::DirectXTex)
        add_library(Microsoft::DirectXTex ALIAS DirectXTex)
    endif()

    # DXC binary package wiring.
    find_path(UPDOWN_DXC_INCLUDE_DIR
        NAMES dxcapi.h
        PATHS "${dxc_bin_SOURCE_DIR}"
        PATH_SUFFIXES include inc
    )
    find_program(UPDOWN_DXC_EXECUTABLE
        NAMES dxc dxc.exe
        PATHS "${dxc_bin_SOURCE_DIR}"
        PATH_SUFFIXES bin bin/x64
    )
    find_library(UPDOWN_DXCOMPILER_LIB
        NAMES dxcompiler
        PATHS "${dxc_bin_SOURCE_DIR}"
        PATH_SUFFIXES lib lib/x64 bin bin/x64
    )
    find_file(UPDOWN_DXCOMPILER_DLL
        NAMES dxcompiler.dll
        PATHS "${dxc_bin_SOURCE_DIR}"
        PATH_SUFFIXES bin bin/x64
    )
    find_file(UPDOWN_DXIL_DLL
        NAMES dxil.dll
        PATHS "${dxc_bin_SOURCE_DIR}"
        PATH_SUFFIXES bin bin/x64
    )

    if(NOT UPDOWN_DXC_INCLUDE_DIR OR NOT UPDOWN_DXC_EXECUTABLE OR NOT UPDOWN_DXCOMPILER_LIB OR NOT UPDOWN_DXCOMPILER_DLL OR NOT UPDOWN_DXIL_DLL)
        message(FATAL_ERROR "Failed to locate DXC assets from FetchContent package.")
    endif()

    if(NOT TARGET Microsoft::DXIL)
        add_library(Microsoft::DXIL SHARED IMPORTED GLOBAL)
        set_target_properties(Microsoft::DXIL PROPERTIES
            IMPORTED_IMPLIB "${UPDOWN_DXCOMPILER_LIB}"
            IMPORTED_LOCATION "${UPDOWN_DXIL_DLL}"
            INTERFACE_INCLUDE_DIRECTORIES "${UPDOWN_DXC_INCLUDE_DIR}"
        )
    endif()

    if(NOT TARGET Microsoft::DirectXShaderCompiler)
        add_library(Microsoft::DirectXShaderCompiler SHARED IMPORTED GLOBAL)
        set_target_properties(Microsoft::DirectXShaderCompiler PROPERTIES
            IMPORTED_IMPLIB "${UPDOWN_DXCOMPILER_LIB}"
            IMPORTED_LOCATION "${UPDOWN_DXCOMPILER_DLL}"
            INTERFACE_INCLUDE_DIRECTORIES "${UPDOWN_DXC_INCLUDE_DIR}"
            INTERFACE_LINK_LIBRARIES "Microsoft::DXIL"
        )
    endif()

    set(UPDOWN_DXC_EXECUTABLE "${UPDOWN_DXC_EXECUTABLE}" CACHE FILEPATH "DXC executable path")
    set(UPDOWN_DXCOMPILER_DLL "${UPDOWN_DXCOMPILER_DLL}" CACHE FILEPATH "dxcompiler runtime dll")
    set(UPDOWN_DXIL_DLL "${UPDOWN_DXIL_DLL}" CACHE FILEPATH "dxil runtime dll")
    set(UPDOWN_DIRECTX_HEADERS_INCLUDE_DIR "${directx_headers_SOURCE_DIR}/include" CACHE PATH "DirectX-Headers include root")

    add_library(updown_deps_ready INTERFACE)
endfunction()
