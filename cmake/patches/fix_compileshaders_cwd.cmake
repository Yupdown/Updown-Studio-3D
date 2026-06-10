# FetchContent PATCH_COMMAND script (cwd = dependency source dir).
#
# DirectXTK12 and DirectXTex invoke their shader-build scripts by bare
# relative name (e.g. `cmake -E env ... CompileShaders.cmd`), relying on
# cwd-based executable lookup. Environments that define
# NoDefaultCurrentDirectoryInExePath (VS Code does for child processes)
# disable that lookup, so the custom build step fails with
# "no such file or directory" (MSB8066). Rewrite the invocations to use
# absolute paths. Each replacement is idempotent: once rewritten, the
# bare-name pattern no longer occurs.
file(READ CMakeLists.txt _content)

# DirectXTK12
string(REPLACE
    " CompileShaders.cmd ARGS"
    " \"\${PROJECT_SOURCE_DIR}/Src/Shaders/CompileShaders.cmd\" ARGS"
    _content "${_content}")

# DirectXTex
string(REPLACE
    " CompileShaders.cmd >"
    " \"\${PROJECT_SOURCE_DIR}/DirectXTex/Shaders/CompileShaders.cmd\" >"
    _content "${_content}")
string(REPLACE
    " hlsl.cmd >"
    " \"\${PROJECT_SOURCE_DIR}/DDSView/hlsl.cmd\" >"
    _content "${_content}")

file(WRITE CMakeLists.txt "${_content}")
