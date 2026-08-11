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

updown_stage_app_runtime(demo)
