set(ENGINE_SOURCES
    source/allocation.cpp
    source/animation_clip.cpp
    source/audio.cpp
    source/audio_clip.cpp
    source/camera.cpp
    source/component.cpp
    source/core.cpp
    source/d3dUtil.cpp
    source/debug_console.cpp
    source/deferred_renderer.cpp
    source/font.cpp
    source/frame_debug.cpp
    source/frame_resource.cpp
    source/gui_button.cpp
    source/gui_element.cpp
    source/gui_image.cpp
    source/gui_text.cpp
    source/inline_mesh_renderer.cpp
    source/input.cpp
    source/light_directional.cpp
    source/material.cpp
    source/mesh.cpp
    source/mesh_base.cpp
    source/mesh_renderer.cpp
    source/mono_upload_buffer.cpp
    source/motion_blur.cpp
    source/pch.cpp
    source/post_process_bloom.cpp
    source/post_process_fxaa.cpp
    source/post_process_outline.cpp
    source/rect_transform.cpp
    source/renderer_base.cpp
    source/resource_load.cpp
    source/resource_object.cpp
    source/rigged_mesh.cpp
    source/rigged_mesh_renderer.cpp
    source/rigged_prop_renderer.cpp
    source/scene.cpp
    source/scene_object.cpp
    source/screen_space_ao.cpp
    source/shader.cpp
    source/shader_compile.cpp
    source/shadow_map.cpp
    source/texture.cpp
    source/time_measure.cpp
    source/transform.cpp
    source/updown_studio.cpp
    source/vertex.cpp
    tracy/public/TracyClient.cpp
)

add_library(engine STATIC ${ENGINE_SOURCES})
add_library(updown::engine ALIAS engine)

target_compile_features(engine PUBLIC cxx_std_20)

target_include_directories(engine
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/source
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/tracy/public
)

target_compile_definitions(engine
    PRIVATE
        UNICODE
        _UNICODE
        _CONSOLE
        CONSOLE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        IMGUI_DEFINE_MATH_OPERATORS
        DIRECTX_TOOLKIT_IMPORT
        $<$<CONFIG:Debug>:_DEBUG>
        $<$<NOT:$<CONFIG:Debug>>:NDEBUG>
        $<$<CONFIG:Profile>:TRACY_ENABLE>
        $<$<CONFIG:Profile>:PROFILE_ENABLE>
)

if(MSVC)
    target_compile_options(engine PRIVATE /W3 /sdl /MP /permissive)
endif()

target_precompile_headers(engine PRIVATE source/pch.h)
set_source_files_properties(
    source/d3dUtil.cpp
    tracy/public/TracyClient.cpp
    PROPERTIES SKIP_PRECOMPILE_HEADERS ON
)

target_link_libraries(engine
    PUBLIC
        d3d12
        dxgi
        d3dcompiler
        dxguid
        runtimeobject
)

find_package(directxtk12 CONFIG REQUIRED)
find_package(directxtex CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_package(directx-dxc CONFIG REQUIRED)

function(link_first_available imported_target visibility)
    foreach(candidate IN LISTS ARGN)
        if(TARGET "${candidate}")
            target_link_libraries(${imported_target} ${visibility} "${candidate}")
            return()
        endif()
    endforeach()
    string(REPLACE ";" ", " candidate_names "${ARGN}")
    message(FATAL_ERROR "No imported target found for: ${candidate_names}")
endfunction()

link_first_available(engine PUBLIC
    Microsoft::DirectXTK12
    directxtk12::directxtk12
    unofficial::directxtk12::directxtk12
)
link_first_available(engine PUBLIC
    Microsoft::DirectXTex
    directxtex::directxtex
    unofficial::directxtex::directxtex
)
link_first_available(engine PUBLIC
    imgui::imgui
    unofficial::imgui::imgui
)
link_first_available(engine PUBLIC
    Microsoft::DirectXShaderCompiler
    directx-dxc::directx-dxc
    unofficial::directx-dxc::directx-dxc
    dxcompiler
)

set(_vcpkg_triplet "${VCPKG_TARGET_TRIPLET}")
if(NOT _vcpkg_triplet)
    set(_vcpkg_triplet "x64-windows")
endif()

set(_vcpkg_installed_dir "${VCPKG_INSTALLED_DIR}")
if(NOT _vcpkg_installed_dir)
    set(_vcpkg_installed_dir "${CMAKE_BINARY_DIR}/vcpkg_installed")
endif()

find_program(ENGINE_DXC_EXECUTABLE
    NAMES dxc dxc.exe
    HINTS
        "${_vcpkg_installed_dir}/${_vcpkg_triplet}/tools/directx-dxc/bin"
        "${_vcpkg_installed_dir}/${_vcpkg_triplet}/tools/directx-dxc"
)
if(NOT ENGINE_DXC_EXECUTABLE)
    message(FATAL_ERROR "dxc executable was not found. Ensure the directx-dxc package is installed by vcpkg.")
endif()

set(ENGINE_COMPILED_SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/source/compiled_shaders")
set(ENGINE_SHADER_COMMON_DEFINES
    KERNEL_SIZE=32
    BLUR_SAMPLE=16
    MAX_BLUR_RADIUS=20
)

function(add_engine_shader hlsl_file entrypoint profile)
    set(options)
    set(oneValueArgs)
    set(multiValueArgs EXTRA_DEFINES)
    cmake_parse_arguments(SHADER "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    get_filename_component(shader_name "${hlsl_file}" NAME_WE)
    set(shader_input "${CMAKE_CURRENT_SOURCE_DIR}/${hlsl_file}")
    set(shader_output "${ENGINE_COMPILED_SHADER_DIR}/${shader_name}.h")
    set(shader_defines)

    foreach(define_item IN LISTS ENGINE_SHADER_COMMON_DEFINES SHADER_EXTRA_DEFINES)
        list(APPEND shader_defines -D${define_item})
    endforeach()

    add_custom_command(
        OUTPUT "${shader_output}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${ENGINE_COMPILED_SHADER_DIR}"
        COMMAND "${ENGINE_DXC_EXECUTABLE}"
            -T ${profile}
            -E ${entrypoint}
            -Fh "${shader_output}"
            -Vn g_cso_${shader_name}
            -I "${CMAKE_CURRENT_SOURCE_DIR}/resource"
            -Zi
            -Qembed_debug
            ${shader_defines}
            "${shader_input}"
        DEPENDS "${shader_input}"
        VERBATIM
        COMMENT "Compiling ${hlsl_file} to ${shader_output}"
    )

    set(ENGINE_SHADER_HEADERS ${ENGINE_SHADER_HEADERS} "${shader_output}" PARENT_SCOPE)
endfunction()

set(ENGINE_SHADER_HEADERS)
add_engine_shader(resource/cs_motion_blur_neighbormax.hlsl CS cs_6_0)
add_engine_shader(resource/cs_motion_blur_tilemax.hlsl CS cs_6_0)
add_engine_shader(resource/cs_screenspace_ao_blur.hlsl CS cs_6_0)
add_engine_shader(resource/ps_bloom_downsample.hlsl PS ps_6_0)
add_engine_shader(resource/ps_bloom_prepass.hlsl PS ps_6_0)
add_engine_shader(resource/ps_bloom_synthesize.hlsl PS ps_6_0)
add_engine_shader(resource/ps_bloom_upsample.hlsl PS ps_6_0)
add_engine_shader(resource/ps_motion_blur_pass.hlsl PS ps_6_0)
add_engine_shader(resource/ps_outline.hlsl PS ps_6_0)
add_engine_shader(resource/ps_screenspace_ao.hlsl PS ps_6_0)
add_engine_shader(resource/ps_fxaa.hlsl FXAAPS ps_6_0
    EXTRA_DEFINES
        FXAA_PC=1
        FXAA_HLSL_5=1
        FXAA_QUALITY__PRESET=12
)
add_engine_shader(resource/vs_drawscreen.hlsl VS vs_6_0)

add_custom_target(engine_compiled_shaders DEPENDS ${ENGINE_SHADER_HEADERS})
add_dependencies(engine engine_compiled_shaders)
target_sources(engine PRIVATE ${ENGINE_SHADER_HEADERS})

function(copy_runtime_dll dll_name)
    find_file(${dll_name}_DEBUG_PATH
        NAMES ${dll_name}.dll
        PATHS "${_vcpkg_installed_dir}/${_vcpkg_triplet}/debug/bin"
        NO_DEFAULT_PATH
    )
    find_file(${dll_name}_RELEASE_PATH
        NAMES ${dll_name}.dll
        PATHS "${_vcpkg_installed_dir}/${_vcpkg_triplet}/bin"
        NO_DEFAULT_PATH
    )

    if(${dll_name}_DEBUG_PATH AND ${dll_name}_RELEASE_PATH)
        add_custom_command(TARGET engine POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<IF:$<CONFIG:Debug>,${${dll_name}_DEBUG_PATH},${${dll_name}_RELEASE_PATH}>"
                "$<TARGET_FILE_DIR:engine>/${dll_name}.dll"
            VERBATIM
        )
    else()
        message(WARNING "${dll_name}.dll was not found in the vcpkg install tree.")
    endif()
endfunction()

copy_runtime_dll(dxil)
copy_runtime_dll(dxcompiler)
copy_runtime_dll(DirectXTK12)
copy_runtime_dll(DirectXTex)
