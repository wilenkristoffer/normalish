# Records the test clips used to judge the relight offline.
#
#   .\tools\record-clips.ps1
#
# Opens a preview window that walks through six poses, one clip each, with the
# instruction on screen and a countdown before every recording. Clips are
# written to recordings\ in the project root, which is gitignored.
#
# Needs about 10 GB of free disk space and 3 GB of free memory at 1080p.

param(
    # Capture preset: 1 = 720p, 2 = 1080p, 3 = 1440p, 4 = 4K.
    [int]$Preset = 2
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root 'build\normalish.exe'
if (-not (Test-Path $exe)) {
    throw "normalish.exe not found. Run .\build.ps1 first."
}

$out = Join-Path $root 'recordings'

Push-Location $root
try { & $exe --record-session $out $Preset } finally { Pop-Location }
