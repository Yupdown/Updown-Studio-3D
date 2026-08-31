set(TESTBED_SOURCES
    main.cpp
    RtTestbed.cpp
    Scenario.cpp
)

set(TESTBED_HEADERS
    RtTestbed.h
    Scenario.h
)

add_executable(rt_testbed WIN32
    ${TESTBED_SOURCES}
    ${TESTBED_HEADERS}
)

target_link_libraries(rt_testbed PRIVATE engine nlohmann_json::nlohmann_json)

target_compile_features(rt_testbed PRIVATE cxx_std_20)

target_compile_definitions(rt_testbed
    PRIVATE
        UNICODE
        _UNICODE
        _WINDOWS
        $<$<CONFIG:Debug>:_DEBUG>
        $<$<NOT:$<CONFIG:Debug>>:NDEBUG>
)

if(MSVC)
    target_compile_options(rt_testbed PRIVATE /W3 /sdl /permissive)
endif()

# Assets resolve through relative resource\ paths, so the testbed always runs from the repo root.
set_target_properties(rt_testbed PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

updown_stage_app_runtime(rt_testbed)
