---
name: rt-visual-test
description: Run the raytracing visual-quality testbed (deterministic Sponza + DamagedHelmet captures with machine-readable artifact checks) and interpret its results. Use after changing the raytracing renderer, denoiser, jitter/reprojection, or shaders to verify no visual artifacts were introduced.
---

# RT visual-quality testbed

`rt_testbed.exe` is a dedicated executable (target `rt_testbed`, sources in [testbed/](testbed/))
separate from the interactive `demo`. It renders one fixed, fully static scene — Khronos **Sponza**
with **DamagedHelmet** in the atrium — from a pinned camera, and measures it.

One command, from the repo root:

```powershell
pwsh scripts/rt-visual-test.ps1            # build + full suite (~3 min on RTX hardware)
pwsh scripts/rt-visual-test.ps1 -Quick     # 64-sample fast pass, for iteration
pwsh scripts/rt-visual-test.ps1 -SkipBuild # reuse the existing Release build
pwsh scripts/rt-visual-test.ps1 -Case aov_motion   # one case (gate always runs first)
pwsh scripts/rt-visual-test.ps1 -SelfTest  # verify the testbed itself catches injected defects
```

The scene assets (~54 MB, not committed) are fetched automatically on first run, or explicitly
with `pwsh scripts/fetch-testbed-assets.ps1`.

## Exit codes

- **0** — all checks passed (with `-SelfTest`: every injected defect was caught)
- **1** — a check failed: read `testbed-results/summary.txt` for the failing lines
- **2** — skipped: DXR unavailable or the raytracer never activated (the engine silently renders
  the raster path in that situation, so nothing visual was actually verified)
- **3** — timeout, capture failure, missing assets, or state machine stuck

## Reading results

- `testbed-results/summary.txt` — one `PASS|FAIL case/check value=... thr=...` line per check.
- `testbed-results/report.json` — the same data structured, plus capture paths and metadata.
- `testbed-results/<case>/*.png` — tonemapped captures, viewable directly with the Read tool.
- `testbed-results/<case>/converged.hdr` — pre-tonemap HDR resolve target (R11G11B10 source).

Values are always emitted even on PASS. On a marginal failure (value close to threshold), look at
the number and the PNG before concluding the renderer regressed — thresholds were calibrated on
one machine and the stochastic ones scale with `--max-samples`.

**Which numbers are stable enough to diff across builds.** `determinism/*` is bit-exact (0.0) and
is the reliable canary: if a change was supposed to be visually neutral and this moves, something
really did change. `primary/*` is NOT bit-stable — it is the first convergence after boot, so
acceleration-structure warm-up perturbs it by ~1e-7 relative *between runs of the same binary*
(`lum_mean` wobbles in its 7th significant digit). Never conclude a regression from `primary`
alone; re-run the same binary first and compare the spread.

## What the cases verify

| Case | Invariant |
|---|---|
| `gate` | DXR is supported AND the raytracer actually replaced the raster path |
| `primary` | no NaN/Inf, no fireflies, sane luminance; the converged image stays put over 16 held frames (temporal stability — catches jitter/reprojection bugs); a-trous on/off shifts image energy < 5% |
| `determinism_ref` / `determinism` | two warm re-convergences over the same RNG/jitter sequence reproduce the same image (the cold boot convergence is excluded: acceleration-structure warm-up shifts silhouettes deterministically) |
| `aov_albedo` / `aov_normal` | debug AOVs finite, structured, in encoded range (bottom 30% of the frame only — the miss shader writes radiance, not the encoded quantity, so sky pixels are excluded) |
| `aov_motion` | motion-vector AOV is uniform 0.5 grey for a static scene (any deviation = motion/jitter-compensation bug) |
| `heatmap` | per-pixel sample count saturates everywhere (accumulation actually progresses) |

## Why this scene

Sponza's arcades give sharp silhouettes and heavy occlusion (where reprojection artifacts appear
first), the open roof puts sky in frame (exercising the miss shader), the raking sun produces a
hard shadow terminator plus a blown highlight on the helmet (where fireflies appear), and the
helmet itself contributes metal/rough PBR with normal maps to the indirect bounce. The bottom of
the frame is always floor, which the AOV range checks depend on.

## Traps

- The camera pose and the scene contents are load-bearing: the committed pose is what the
  thresholds are calibrated against. `--pose x,y,z,yaw,pitch` overrides it for exploratory runs
  (re-tuning against captured PNGs without a rebuild) — results from such a run are not comparable.
- The testbed hard-terminates after writing results to dodge a pre-existing engine teardown crash
  (0xC0000005 on window close, present in the interactive demo too). Don't add teardown-dependent
  logic after `FinishSuite`.
- Anything that touches `RadianceSettings` (fog, sun, spp, debug mode) mid-case drops the
  accumulation history; settings may only change in `LoadCase`. `RaytracingAtrousIterations` is
  the deliberate exception (display-side only).
- Threshold constants and their rationale live in [testbed/RtTestbed.cpp](testbed/RtTestbed.cpp)
  (`namespace thresholds`).
