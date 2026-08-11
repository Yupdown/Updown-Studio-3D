# Runs the raytracing visual-quality testbed (rt_testbed.exe: Sponza + DamagedHelmet, fixed
# camera) and prints its summary.
#
#   pwsh scripts/rt-visual-test.ps1 [-SkipBuild] [-Quick] [-SelfTest] [-Case NAME] [-OutDir DIR]
#
# The scene assets are fetched on demand by scripts/fetch-testbed-assets.ps1.
#
# Exit codes (from rt_testbed.exe):
#   0  all checks passed
#   1  a check failed (or, with -SelfTest, an injected defect went uncaught)
#   2  skipped: DXR unavailable or the raytracer never activated
#   3  timeout / internal error
#
# Results land in <OutDir>/report.json (numbers), <OutDir>/summary.txt (one line per check)
# and <OutDir>/<case>/*.png|*.hdr (viewable captures).

param(
    [switch]$SkipBuild,
    [switch]$Quick,
    [switch]$SelfTest,
    [string]$Case = "",
    [string]$OutDir = "testbed-results",
    [int]$TimeoutSec = 600
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot

# The scene is built from Khronos sample assets that are too large to commit.
if (-not (Test-Path (Join-Path $repoRoot "resource\model\sponza\Sponza.gltf"))) {
    Write-Host "Testbed assets missing; fetching them once..."
    & (Join-Path $PSScriptRoot "fetch-testbed-assets.ps1")
    if ($LASTEXITCODE -ne 0) {
        Write-Host "ASSET FETCH FAILED"
        exit 3
    }
}

if (-not $SkipBuild) {
    Push-Location $repoRoot
    try {
        cmake --build --preset build-release --target rt_testbed
        if ($LASTEXITCODE -ne 0) {
            Write-Host "BUILD FAILED"
            exit 3
        }
    }
    finally {
        Pop-Location
    }
}

$exePath = Join-Path $repoRoot "build\vs2026-x64\testbed\Release\rt_testbed.exe"
if (-not (Test-Path $exePath)) {
    Write-Host "rt_testbed.exe not found at $exePath"
    exit 3
}

$resolvedOut = if ([System.IO.Path]::IsPathRooted($OutDir)) { $OutDir } else { Join-Path $repoRoot $OutDir }
if (Test-Path $resolvedOut) {
    Remove-Item -Recurse -Force $resolvedOut
}

$argList = @("--out", $resolvedOut)
if ($Quick) { $argList += "--quick" }
if ($SelfTest) { $argList += "--self-test" }
if ($Case -ne "") { $argList += @("--case", $Case) }

# cwd must be the repo root: the demo loads assets through relative resource\ paths.
$process = Start-Process -FilePath $exePath -ArgumentList $argList -WorkingDirectory $repoRoot -PassThru
if (-not $process.WaitForExit($TimeoutSec * 1000)) {
    $process.Kill()
    Write-Host "TIMEOUT after $TimeoutSec s -- process killed"
    exit 3
}
$exitCode = $process.ExitCode

$summaryPath = Join-Path $resolvedOut "summary.txt"
if (Test-Path $summaryPath) {
    Get-Content $summaryPath
}
else {
    Write-Host "No summary written ($summaryPath missing) -- the run died before finishing."
}
exit $exitCode
