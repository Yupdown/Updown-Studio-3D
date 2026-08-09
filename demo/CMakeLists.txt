set(DEMO_SOURCES
    demo.cpp
    MCTerrainGenerator.cpp
    MCTilemap.cpp
    MCTilemapMeshGenerator.cpp
)

set(DEMO_HEADERS
    CustomMath.hpp
    framework.h
    MCTerrainGenerator.h
    MCTilemap.h
    MCTilemapMeshGenerator.h
    PerlinNoise.hpp
    Resource.h
    Vector2D.hpp
    Vector3D.hpp
)

add_executable(demo WIN32
    ${DEMO_SOURCES}
    ${DEMO_HEADERS}
)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/demo.rc")
    target_sources(demo PRIVATE demo.rc)
endif()

target_link_libraries(demo PRIVATE engine)

target_compile_features(demo PRIVATE cxx_std_20)

target_compile_definitions(demo
    PRIVATE
        UNICODE
        _UNICODE
        _WINDOWS
        $<$<CONFIG:Debug>:_DEBUG>
        $<$<NOT:$<CONFIG:Debug>>:NDEBUG>
)

if(MSVC)
    target_compile_options(demo PRIVATE /W3 /sdl /permissive)
    target_link_options(demo PRIVATE /STACK:4294967296)
endif()

set_target_properties(demo PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/demo"
)

add_custom_command(TARGET demo POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_RUNTIME_DLLS:demo>
        $<TARGET_FILE_DIR:demo>
    COMMAND_EXPAND_LISTS
    VERBATIM
)

# Streamline runtime. These are copied here rather than picked up by TARGET_RUNTIME_DLLS, because
# manual hooking means nothing links against them -- sl.interposer.dll is loaded at runtime, so as
# far as the linker is concerned they are not dependencies at all.
#
# sl.interposer and sl.common are mandatory for any feature; sl.dlss_d plus its NGX back end are
# what DLSS Ray Reconstruction runs on. These are the signed production binaries from bin/x64: the
# unsigned development builds under bin/x64/development trip Streamline's own signature check and
# must never ship.
if(UPDOWN_STREAMLINE_BIN_DIR)
    set(UPDOWN_STREAMLINE_RUNTIME_FILES
        sl.interposer.dll
        sl.common.dll
        sl.dlss_d.dll
        nvngx_dlssd.dll
        nvngx_dlss.license.txt
    )
    foreach(_sl_file IN LISTS UPDOWN_STREAMLINE_RUNTIME_FILES)
        if(NOT EXISTS "${UPDOWN_STREAMLINE_BIN_DIR}/${_sl_file}")
            message(FATAL_ERROR "Streamline package is missing ${_sl_file}.")
        endif()
        add_custom_command(TARGET demo POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${UPDOWN_STREAMLINE_BIN_DIR}/${_sl_file}"
                "$<TARGET_FILE_DIR:demo>/${_sl_file}"
            VERBATIM
        )
    endforeach()
endif()

# texconv (built by the DirectXTex dependency) performs GPU texture compression at runtime.
# Build it before the demo and copy it next to the executable, where the engine resolves it.
if(TARGET texconv)
    add_dependencies(demo texconv)
    add_custom_command(TARGET demo POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:texconv>
            $<TARGET_FILE_DIR:demo>
        VERBATIM
    )
else()
    message(WARNING "texconv target not found; runtime texture compression will fail. Ensure the DirectXTex dependency builds its command-line tools.")
endif()
