set(SCENE_EXPORT_SOURCES
    source/animation_clip_exporter.cpp
    source/exporter_base.cpp
    source/main.cpp
    source/rigged_mesh_exporter.cpp
    source/static_mesh_exporter.cpp
    source/vertex.cpp
)

set(SCENE_EXPORT_HEADERS
    source/animation_clip_exporter.h
    source/exporter_base.h
    source/rigged_mesh_exporter.h
    source/static_mesh_exporter.h
    source/vertex.h
)

add_executable(SceneExport
    ${SCENE_EXPORT_SOURCES}
    ${SCENE_EXPORT_HEADERS}
)

target_compile_features(SceneExport PRIVATE cxx_std_20)
target_include_directories(SceneExport PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source)

target_compile_definitions(SceneExport
    PRIVATE
        UNICODE
        _UNICODE
        _CONSOLE
        $<$<CONFIG:Debug>:_DEBUG>
        $<$<NOT:$<CONFIG:Debug>>:NDEBUG>
)

if(MSVC)
    target_compile_options(SceneExport PRIVATE /W3 /sdl)
endif()

# SceneExport uses its own vcpkg manifest/install tree in this repository.
# When the top-level configure does not use a toolchain/manifest that includes
# assimp, prepend the local package prefix so find_package works.
set(_sceneexport_local_vcpkg_prefix
    "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg_installed/x64-windows/x64-windows"
)
if(EXISTS "${_sceneexport_local_vcpkg_prefix}/share/assimp/assimpConfig.cmake")
    list(PREPEND CMAKE_PREFIX_PATH "${_sceneexport_local_vcpkg_prefix}")
endif()

find_package(assimp CONFIG REQUIRED)
target_link_libraries(SceneExport PRIVATE assimp::assimp)

if(WIN32)
    add_custom_command(TARGET SceneExport POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:SceneExport>
            $<TARGET_FILE_DIR:SceneExport>
        COMMAND_EXPAND_LISTS
        VERBATIM
    )
endif()
