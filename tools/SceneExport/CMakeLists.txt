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

if(TARGET assimp::assimp)
    target_link_libraries(SceneExport PRIVATE assimp::assimp)
elseif(TARGET assimp)
    target_link_libraries(SceneExport PRIVATE assimp)
else()
    message(FATAL_ERROR "assimp target was not prepared by FetchContent.")
endif()

