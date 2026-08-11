# Downloads the glTF sample assets the raytracing testbed scene is built from.
#
#   pwsh scripts/fetch-testbed-assets.ps1 [-Force]
#
# The assets (~54 MB) are not committed: they are unmodified Khronos sample models, fetched into
#   resource/model/sponza/Sponza.gltf      (+ .bin and 69 textures beside it)
#   resource/model/DamagedHelmet.glb
# Both are CC-BY licensed; see the LICENSE.md fetched alongside Sponza.
#
# Idempotent: existing files are skipped unless -Force is passed.

param([switch]$Force)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$modelDir = Join-Path $repoRoot "resource\model"
$sponzaDir = Join-Path $modelDir "sponza"

$rawBase = "https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models"
$apiBase = "https://api.github.com/repos/KhronosGroup/glTF-Sample-Assets/contents/Models"

function Get-File($url, $destination) {
    if ((Test-Path $destination) -and -not $Force) {
        return $false
    }
    $dir = Split-Path -Parent $destination
    if (-not (Test-Path $dir)) {
        New-Item -ItemType Directory -Force $dir | Out-Null
    }
    Invoke-WebRequest -Uri $url -OutFile $destination -TimeoutSec 120
    return $true
}

Write-Host "Fetching DamagedHelmet..."
$helmet = Join-Path $modelDir "DamagedHelmet.glb"
if (Get-File "$rawBase/DamagedHelmet/glTF-Binary/DamagedHelmet.glb" $helmet) {
    Write-Host "  DamagedHelmet.glb"
} else {
    Write-Host "  DamagedHelmet.glb (already present)"
}

Write-Host "Fetching Sponza (71 files, ~50 MB)..."
# The file list is queried rather than hardcoded: Sponza's textures have opaque hashed names.
$listing = Invoke-RestMethod -Uri "$apiBase/Sponza/glTF" -TimeoutSec 60
$downloaded = 0
$skipped = 0
foreach ($entry in $listing) {
    if ($entry.type -ne "file") { continue }
    $dest = Join-Path $sponzaDir $entry.name
    if (Get-File $entry.download_url $dest) {
        $downloaded++
        Write-Host "  [$downloaded] $($entry.name)"
    } else {
        $skipped++
    }
}
Get-File "$rawBase/Sponza/LICENSE.md" (Join-Path $sponzaDir "LICENSE.md") | Out-Null

Write-Host ""
Write-Host "Done: $downloaded downloaded, $skipped already present."
if (-not (Test-Path (Join-Path $sponzaDir "Sponza.gltf"))) {
    Write-Host "ERROR: Sponza.gltf is missing after the fetch."
    exit 1
}
if (-not (Test-Path $helmet)) {
    Write-Host "ERROR: DamagedHelmet.glb is missing after the fetch."
    exit 1
}
exit 0
