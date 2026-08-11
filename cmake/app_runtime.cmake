# Stages everything an executable needs beside it at runtime: linked dependency DLLs, the
# Streamline runtime, and texconv. Shared by every app target (demo, testbed) so they cannot
# drift apart.
function(updown_stage_app_runtime target)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:${target}>
            $<TARGET_FILE_DIR:${target}>
        COMMAND_EXPAND_LISTS
        VERBATIM
    )

    # Streamline runtime. These are copied here rather than picked up by TARGET_RUNTIME_DLLS,
    # because manual hooking means nothing links against them -- sl.interposer.dll is loaded at
    # runtime, so as far as the linker is concerned they are not dependencies at all.
    #
    # sl.interposer and sl.common are mandatory for any feature; sl.dlss_d plus its NGX back end
    # are what DLSS Ray Reconstruction runs on. These are the signed production binaries from
    # bin/x64: the unsigned development builds under bin/x64/development trip Streamline's own
    # signature check and must never ship.
    if(UPDOWN_STREAMLINE_BIN_DIR)
        set(_streamline_files
            sl.interposer.dll
            sl.common.dll
            sl.dlss_d.dll
            nvngx_dlssd.dll
            nvngx_dlss.license.txt
        )
        foreach(_sl_file IN LISTS _streamline_files)
            if(NOT EXISTS "${UPDOWN_STREAMLINE_BIN_DIR}/${_sl_file}")
                message(FATAL_ERROR "Streamline package is missing ${_sl_file}.")
            endif()
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${UPDOWN_STREAMLINE_BIN_DIR}/${_sl_file}"
                    "$<TARGET_FILE_DIR:${target}>/${_sl_file}"
                VERBATIM
            )
        endforeach()
    endif()

    # texconv (built by the DirectXTex dependency) performs GPU texture compression at runtime.
    # Build it first and copy it next to the executable, where the engine resolves it.
    if(TARGET texconv)
        add_dependencies(${target} texconv)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:texconv>
                $<TARGET_FILE_DIR:${target}>
            VERBATIM
        )
    else()
        message(WARNING "texconv target not found; runtime texture compression will fail. Ensure the DirectXTex dependency builds its command-line tools.")
    endif()
endfunction()
