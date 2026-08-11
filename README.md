# Updown-Studio-3D

A DirectX 12 3D engine project built with CMake and FetchContent dependencies.

## Build (Windows, Visual Studio 2026)

```powershell
cmake --preset vs2026-x64
cmake --build --preset build-debug
```

Targets:
- `engine` static library
- `demo` executable — interactive sandbox
- `rt_testbed` executable — raytracing visual-quality testbed (see below)

## Raytracing visual-quality testbed

`rt_testbed` is a dedicated, non-interactive executable that renders one fixed scene — Khronos
**Sponza** with **DamagedHelmet** in the atrium — from a pinned camera and checks it for visual
artifacts. It is separate from `demo` on purpose: the demo is a sandbox whose content changes
freely, while a verification baseline has to stay comparable across commits.

Cases cover converged-image health (NaN/Inf, fireflies, luminance), temporal stability,
re-convergence determinism, debug-AOV invariants, and sample-count saturation. Each run writes
PNG/HDR captures plus machine-readable results.

```powershell
pwsh scripts/rt-visual-test.ps1           # build + full suite
pwsh scripts/rt-visual-test.ps1 -Quick    # fast pass for iteration
pwsh scripts/rt-visual-test.ps1 -SelfTest # prove the checks catch injected defects
```

Scene assets (~54 MB) are not committed; the runner fetches them on first use, or run
`pwsh scripts/fetch-testbed-assets.ps1` explicitly.

Exit codes: `0` pass, `1` check failed, `2` skipped (no DXR — nothing visual was verified),
`3` timeout/missing assets/internal error. Results: `testbed-results/report.json`, `summary.txt`
and per-case `*.png` / `*.hdr` captures. Details in `.claude/skills/rt-visual-test/SKILL.md` and
`testbed/RtTestbed.cpp`.
