---
name: rt-visual-test
description: Run the raytracing visual-quality testbed (scenario-driven captures with machine-readable checks) and interpret its results. Use after changing the raytracing renderer, denoiser, jitter/reprojection, or shaders to verify no visual artifacts were introduced — and use a custom scenario file whenever you need deterministic target screenshots of any scene/pose/setting. Verification goes through the testbed only; the interactive demo is never launched or driven for it.
---

# RT visual-quality testbed

`rt_testbed.exe` is a dedicated executable (target `rt_testbed`, sources in [testbed/](testbed/))
separate from the interactive `demo`. Everything it renders and measures is described by a
**scenario file**; the default is the committed regression suite
[testbed/scenarios/rt-suite.json](testbed/scenarios/rt-suite.json) — the reference Sponza +
DamagedHelmet scene from the pinned camera. **Suite cases are edited in that JSON, not in C++.**

**Verify with the testbed only. Never launch or drive `demo.exe` to check a change.** Anything
the demo can show — a pose, a debug view, a render option, the DLSS Ray Reconstruction path, a
long-running temporal chain — is expressed as a scenario case and read back as an HDR capture
plus `stat_*` lines. If something genuinely cannot be reached from a scenario, add the missing
case-level key or render option to the testbed rather than reaching for the demo window.
Thresholds and check logic stay calibrated in [testbed/RtTestbed.cpp](testbed/RtTestbed.cpp)
(`namespace thresholds`); a scenario chooses structure, never calibration.

One command, from the repo root:

```powershell
pwsh scripts/rt-visual-test.ps1            # build + full suite (~3 min on RTX hardware)
pwsh scripts/rt-visual-test.ps1 -Quick     # 64-sample fast pass, for iteration
pwsh scripts/rt-visual-test.ps1 -SkipBuild # reuse the existing Release build
pwsh scripts/rt-visual-test.ps1 -Case aov_motion   # one case (gate always runs first)
pwsh scripts/rt-visual-test.ps1 -SelfTest  # verify the testbed itself catches injected defects
pwsh scripts/rt-visual-test.ps1 -Scenario testbed/scenarios/helmet-basic.json  # custom scenario
pwsh scripts/rt-visual-test.ps1 -Scenario my.json -MaxSamples 64 -SkipBuild    # fast custom run
```

The suite's scene assets (~54 MB, not committed) are fetched automatically on first run, or
explicitly with `pwsh scripts/fetch-testbed-assets.ps1`.

## Scenario files: deterministic captures of anything

To get target screenshots of a new model, pose, or setting: write a small JSON file, run it,
read the PNGs. No rebuild, no window interaction. Annotated examples:
[helmet-basic.json](testbed/scenarios/helmet-basic.json) (works after asset fetch),
[dragon-poses.json](testbed/scenarios/dragon-poses.json) (multi-pose + overrides, local asset).

```jsonc
{
  "schemaVersion": 1,                    // required
  "name": "my-check",                    // optional; report.json "scene". Default: file stem
  "scene": {                             // omitted entirely => reference Sponza+helmet scene
    "models": [{ "path": "resource/model/DamagedHelmet.glb",   // resource-relative
                 "position": [0,1.3,0], "scale": 0.9,          // scale: scalar or [x,y,z]
                 "rotationYawPitchRoll": [1.5708,-1.5708,0],   // radians
                 "enableRaytracing": true }],
    "light": { "intensity": 26.7, "color": [1,1,1],
               "yawPitch": [0.9,1.15], "angularDiameterDegrees": 0.53 },
    "environment": "resource/texture/....hdr"    // null => no environment map
  },
  "camera": { "position": [-7,3.2,0], "yaw": 1.5708, "pitch": 0.10,
              "fovDegrees": 60, "near": 0.05, "far": 200 },
  "cases": [{
    "name": "front",                     // [A-Za-z0-9_-]{1,64}, unique; names the output dir
    "evaluator": "captureOnly",          // default; see the registry below
    "debugMode": "none",                 // none|albedo|normal|directOnly|indirectOnly|motionVector|
                                         // sampleHeatmap|metallicRoughness|emission|specularOnly|brdfFurnace
    "denoiser": "builtin",               // builtin (default, deterministic) | off (raw per-frame estimate,
                                         // exactly what a denoiser is fed) | dlssRayReconstruction
    "convergeFrames": -1,                // -1 => MaxSamples + 64
    "renderHeight": 0,                   // 0 = display res; else 32..8192
    "hold": false,                       // + "hold" capture after 16 still frames
    "motionBlurCoverage": false,         // converge/nudge/replay flow, back-buffer captures
    "skipOnQuick": false,                // dropped under -Quick
    "requires": [],                      // cases -Case pulls in along with this one
    "pose": { "position": [0,1.5,-3], "yaw": 0, "pitch": 0 },   // per-case camera pose
    "renderOptions": { "skyMaxRadiance": 1000.0 }               // whitelist, applied per case
  }]
}
```

Rules that matter in practice:

- **Strict parsing.** Any unknown key, wrong type, or out-of-range value exits 3 with the key
  named in `summary.txt`. `//` comments are allowed.
- **Radians vs degrees**: rotations/yaw/pitch are radians; anything with `Degrees` in the name
  (FOV, sun angular diameter) is degrees.
- **Model transforms are optional overrides**: an omitted `position`/`scale`/`rotationYawPitchRoll`
  leaves the instantiated root's own TRS (from the asset) untouched — Sponza's root transform is
  load-bearing, so the suite scene depends on this.
- **Units of pose**: yaw 0 looks toward +Z, positive yaw turns toward +X, positive pitch looks
  down.
- `renderOptions` is a typed whitelist (`samplesPerPixel`, `maxSamplesMoving`, fog fields,
  `skyMaxRadiance`, `specularFireflyClamp`, `restirGi`, `restirSpatialSamples`,
  `restirSpatialRadius`, `restirTemporalMClamp`, `restirPermutation`, `restirBoilingStrength`, ...).
  Harness-owned fields are rejected as unknown keys on purpose: `drawRaytracing`,
  `maxSamplesStatic` (that's `-MaxSamples`), the motion-blur toggles, bloom. The denoiser is a
  case-level `denoiser` key, not a render option: the suite's baseline forces the built-in one,
  and a case that names `dlssRayReconstruction` or `off` gets it for that case only.
- The renderer falls back to the built-in denoiser silently (no Streamline, unsupported GPU,
  fisheye, any debug view), so every captureOnly case reports `stat_denoiser` (0 off, 1 built-in,
  2 DLSS-RR): read it before trusting a Ray Reconstruction measurement.
- The `gate` case (DXR actually active) is implicit, always first; the name is reserved.
- Each case re-runs from a clean state (history invalidated, RNG/jitter counter reset), so
  per-case poses and overrides never contaminate the next case.
- One scene per run. Different scene = different scenario file = separate run.
- `--pose x,y,z,yaw,pitch` (exe flag) still overrides **every** case's pose — exploratory only.
- `-SelfTest` requires the scenario to contain a case named `primary` with evaluator `primary`
  (the default suite does).
- The global frame budget is 20000 frames: many cases x large `convergeFrames` at high
  `-MaxSamples` can hit it (exit 3, "state machine stuck"). Use `-MaxSamples 64` for capture runs.
- Relative `--scenario`/`--out` paths resolve against the launch directory (the scenario is
  parsed before the engine moves the cwd to the repo root). The runner script and the VS debugger
  both launch from the repo root, so this only bites bare-exe runs from elsewhere.

## Evaluators

`evaluator` picks the check function; all but `captureOnly` are the calibrated suite checks:
`primary` (needs `hold`), `determinismRef`/`determinism` (the latter needs an
earlier ref case), `aovAlbedo`, `aovNormal`, `aovMotion`, `aovMaterial`, `aovEmission`,
`aovFurnace`, `aovSpecular`, `heatmap`, `motionBlurCoverage` (needs the flag of the same name).
They are calibrated against the default scene and pose — pointing them at a custom scene is
allowed but the thresholds no longer mean what they meant.

`captureOnly` (the default) gates only on real defects — `capture_valid` (the readback produced
an image) and `nan_inf` — and reports everything else as **informational stats**: summary lines
like `PASS front/stat_lum_mean value=0.42 thr=null`. `stat_*` + `thr=null` = a measurement, not
a check. With `hold`/`motionBlurCoverage` it also emits pair-diff stats
(`stat_temporal_*`, `stat_blur_*`). `stat_lum_stddev` is a firefly meter, not a
noise meter; for noise or bias, load the `converged.hdr` files, box-blur them and compare against
a 64-spp single-frame reference case (`"samplesPerPixel": 64` with `-MaxSamples 1`).

## Exit codes

- **0** — all checks passed (with `-SelfTest`: every injected defect was caught)
- **1** — a check failed: read `testbed-results/summary.txt` for the failing lines
- **2** — skipped: DXR unavailable or the raytracer never activated (the engine silently renders
  the raster path in that situation, so nothing visual was actually verified)
- **3** — scenario/setup error, timeout, capture failure, or state machine stuck (the ERROR line
  in summary.txt says which)

## Reading results

- `testbed-results/summary.txt` — one `PASS|FAIL case/check value=... thr=...` line per check.
- `testbed-results/report.json` — the same data structured, plus capture paths, the scenario
  path, and metadata.
- `testbed-results/<case>/*.png` — tonemapped captures, viewable directly with the Read tool.
- `testbed-results/<case>/converged.hdr` — pre-tonemap HDR resolve target (R11G11B10 source).
- `testbed-results/motion_blur_*/blur_off.png` and `blur_on.png` — the same frame with the blur
  off and on. The bug these guard is obvious by eye: a hard seam where the blur stops, at
  `motion_blur_render_extent` (reported as a value/threshold pair for exactly this reason).

Values are always emitted even on PASS. On a marginal failure (value close to threshold), look at
the number and the PNG before concluding the renderer regressed — thresholds were calibrated on
one machine and the stochastic ones scale with `-MaxSamples`.

**Which numbers are stable enough to diff across builds.** `determinism/*` is bit-exact (0.0) and
is the reliable canary: if a change was supposed to be visually neutral and this moves, something
really did change. `primary/*` is NOT bit-stable — it is the first convergence after boot, so
acceleration-structure warm-up perturbs it by ~1e-7 relative *between runs of the same binary*
(`lum_mean` wobbles in its 7th significant digit). Never conclude a regression from `primary`
alone; re-run the same binary first and compare the spread. The same applies to a custom
scenario's `stat_lum_*` between two process runs: agreement to ~5-6 significant digits is
expected, bit-exact PNGs are not (within-process warm-vs-warm is the only bit-exact comparison).
`primary/temporal_*` is the loosest of the lot — it is a small difference of nearly equal
images, so it amplifies that wobble into its 6th significant digit.

Anything read from a capture is quantised by the HDR target's `R11G11B10_FLOAT`: 6 mantissa bits
on R and G, 5 on B. Near 1.0 that is a quantum of ~0.008, so `aov_furnace`'s bounds resolve energy
errors to about a percent regardless of how many digits they print.

## What the suite cases verify

| Case | Invariant |
|---|---|
| `gate` | DXR is supported AND the raytracer actually replaced the raster path |
| `primary` | no NaN/Inf, no fireflies, sane luminance; the converged image stays put over 16 held frames (temporal stability — catches jitter/reprojection bugs) |
| `determinism_ref` / `determinism` | two warm re-convergences over the same RNG/jitter sequence reproduce the same image (the cold boot convergence is excluded: acceleration-structure warm-up shifts silhouettes deterministically) |
| `aov_albedo` / `aov_normal` | debug AOVs finite, structured, in encoded range (bottom 30% of the frame only — the miss shader writes radiance, not the encoded quantity, so sky pixels are excluded) |
| `aov_motion` | motion-vector AOV is uniform 0.5 grey for a static scene (any deviation = motion/jitter-compensation bug) |
| `aov_material` | roughness **varies** across the frame. A constant channel — which is what this measured before the material table existed — fails it. |
| `aov_emission` | something in the frame emits. Sponza has no emissive and DamagedHelmet does, so a non-zero peak is threshold-free proof that emissive reaches the shader; the sky is excluded from this view shader-side. |
| `aov_furnace` | the specular lobe's directional albedo, with F0 forced to 1. **Traces nothing** — it isolates D, V, F and the VNDF sampler from transport, so a failure here is a BRDF bug and can be nothing else. Must not exceed 1 (energy creation) and must reach ~1 somewhere (the narrowest lobe in frame must conserve). |
| `aov_specular` | the specular indirect bounce alone: present, bounded by the firefly clamp, and structured. Whole-frame, unlike the other AOV checks — Sponza's floor is a rough dielectric, so the usual bottom-rows restriction would sample the least specular surfaces in the scene. |
| `heatmap` | per-pixel sample count saturates everywhere (accumulation actually progresses) |
| `motion_blur_native` / `motion_blur_scaled` | the screen-space motion blur reaches the **whole** frame, including when the raytracer renders below the display resolution. The only checks that measure a post-process rather than the raytracer, and so the only ones read off the back buffer. |

`aov_material` is also worth *looking* at: Sponza's stone and fabric read green (rough dielectric),
the helmet reads red (smooth metal) and the drapes' gold embroidery reads orange. Those are glTF's
own semantics, so a channel swizzle regression is obvious by eye as well as by the stddev check.

So is `aov_furnace`: it should show a clear gradient — rough drapes visibly dark (single-scattering
GGX genuinely loses energy at high roughness; that deficit is expected, not a bug), the helmet's
visor and the stone near white. A *flat* furnace image means roughness is not reaching the BRDF.
Black speckle at silhouettes is the `NoV <= 0` guard, not an artifact.

## Why the default scene

Sponza's arcades give sharp silhouettes and heavy occlusion (where reprojection artifacts appear
first), the open roof puts sky in frame (exercising the miss shader), the raking sun produces a
hard shadow terminator plus a blown highlight on the helmet (where fireflies appear), and the
helmet itself contributes metal/rough PBR with normal maps to the indirect bounce. The bottom of
the frame is always floor, which the AOV range checks depend on.

## Traps

- The default scene/camera values live as the `Scenario` struct defaults in
  [testbed/Scenario.h](testbed/Scenario.h) — the committed pose is what the suite thresholds are
  calibrated against. `--pose` overrides it for exploratory runs; results from such a run are not
  comparable.
- The testbed hard-terminates after writing results to dodge a pre-existing engine teardown crash
  (0xC0000005 on window close, present in the interactive demo too). Don't add teardown-dependent
  logic after `FinishSuite`, and scenario parse failures terminate via `FailSetup` the same way.
- Anything that touches `RadianceSettings` (fog, sun, spp, debug mode) mid-case drops the
  accumulation history; the harness therefore applies scenario `renderOptions` only while loading
  a case.
- The built-in denoiser is **temporal accumulation only**. Spatial locality comes from ReSTIR GI's
  resampling upstream (`restirGi`, on by default); there is no spatial filter to toggle, so a
  noise comparison is a ReSTIR on/off pair, not a filter on/off pair.
- `DrawMotionBlur` is off for every case **except** the `motionBlurCoverage` ones, which also
  raise `MotionBlurShutterSpeed`. Between cases the whole `RenderOptions` baseline is restored,
  so overrides never leak forward.
- Measuring a post-process means capturing the **back buffer**, not `HdrTarget`. `HdrTarget` is
  the raytracing resolve, written before bloom/tonemapping and before motion blur — a check
  reading it would see a frame the post-process chain has not touched yet and pass on anything.
- `MotionBlurFactor = MotionBlurShutterSpeed / deltaTime`, so blur length depends on how fast the
  machine is running. The testbed's frames are slow and uneven (queue flushes, readback stalls),
  and at the shipped shutter speed the blur fell under the pixel shader's half-pixel early-out and
  the case silently measured nothing. Anything time-scaled needs its input pinned hard enough to
  saturate, not merely to be large.
- The motion blur cases need the camera to *move*, which the rest of the suite never does. They
  converge, turn one frame off the pinned pose, capture, then replay that run bit-for-bit with the
  blur on. Both halves reset the frame counter **and** invalidate history at the same point, so
  their pre-blur images are identical and the difference is the blur alone.
- Editing a shader with `Copy-Item` does not rebuild it. `Copy-Item` preserves the source's
  timestamp, so a file restored from a backup can be *older* than the generated header in
  `build/.../generated/compiled_shaders/` and MSBuild will skip it — leaving the previous shader in
  the binary while the working tree shows the new one. Results from such a run look like a fix that
  did not work. Touch the file (or delete the header) after restoring one.
- Threshold constants and their rationale live in [testbed/RtTestbed.cpp](testbed/RtTestbed.cpp)
  (`namespace thresholds`).
- The suite runs the **raytracer only** — `DrawRaytracing` is forced on and the gate skips
  everything if it is not. Nothing here covers the deferred raster path, and its lighting shader is
  compiled at *runtime* from `resource/shader/color.hlsl`, so a successful build proves nothing
  about it. Raster changes are outside this harness; say so and leave the interactive check to
  the user rather than launching the demo.
- `hold` writes only a PNG for its second capture. When two captures must be compared in HDR
  (a pattern's persistence, a temporal chain's drift), use two cases whose `convergeFrames`
  differ by the gap: cases restart from the same reset state, so frame N of one case is frame N
  of the other, and the second case's capture is exactly the later frame of the same chain.
- ImGui panel positions come from the build folder's `imgui.ini`, camera state from whoever
  touched the window last, and a foreground window re-reads the real cursor: nothing about a
  demo capture is reproducible, which is the other reason it is not used for verification.
- The suite pose is static, so nothing here exercises view-dependent history validation. History
  is validated geometrically; a camera rotating in place passes every geometric test while the
  specular lobe sweeps across the surface. That failure mode needs a rotating-camera check.
